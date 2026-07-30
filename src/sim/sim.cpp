#include "config.h"
#include "state.h"
#include "trig.h"

// The deterministic simulation.
//
// Rules this file obeys without exception:
//   1. No floats. Fixed-point only. (Melee itself uses float + atan2f/sqrtf
//      freely -- it was a local-only game, so cross-machine determinism was never
//      a requirement. For an online game it is, so floats stay out.)
//   2. No numeric literals. Every tunable lives in config.h.
//   3. No wall-clock reads, no rand(), no I/O, no allocation.
//   4. Players resolve in fixed index order, never any other ordering.
// Break any of these and rollback desyncs in ways that are miserable to debug,
// because the symptom shows up seconds after the cause.

namespace tf {

namespace {

// Named individually rather than `using namespace config` -- a blanket import
// collides with the tf-level aliases (kMaxPlayers exists in both scopes).
using config::ATK_AIR_BACK;
using config::ATK_AIR_DOWN;
using config::ATK_AIR_FORWARD;
using config::ATK_AIR_NEUTRAL;
using config::ATK_AIR_UP;
using config::ATK_COUNT;
using config::ATK_JAB;
using config::ATK_NONE;
using config::ATK_SMASH_DOWN;
using config::ATK_SMASH_SIDE;
using config::ATK_SMASH_UP;
using config::ATK_TILT_DOWN;
using config::ATK_TILT_SIDE;
using config::ATK_TILT_UP;
using config::kDI;
using config::kHitlag;
using config::kHitstun;
using config::kKnockback;
using config::Knockback;
using config::Knockdown;
using config::kRespawn;
using config::kSDI;
using config::kShieldBreak;
using config::kSmash;
using config::kStick;
using config::kThrow;
using config::Ledge;

// Per-player character lookup. EVERY character constant in the simulation is read
// through these -- there is NO global fighter. This mirrors how Melee does it:
// attributes hang off the fighter (fp->co_attrs.grav, fp->co_attrs.weight) and are
// passed into the physics functions rather than consulted as globals.
//
// The bounds check is not paranoia: charId arrives from the network, and an
// out-of-range value must produce a defined result rather than reading past the
// roster. A wrong character is recoverable; undefined behavior is not.
inline const Fighter &fighterOf(const Player &p) {
    return config::kFighters[p.charId < config::CHAR_COUNT ? p.charId : config::CHAR_SCOUT];
}

// Attack tables belong to the character, so the same AttackId resolves to
// different frame data for different fighters.
inline const AttackData &attackOf(const Player &p, uint8_t id) {
    const AttackData *table = fighterOf(p).attacks;
    return table[id < config::ATK_COUNT ? id : config::ATK_NONE];
}

void enterState(Player &p, ActionState s) {
    p.state = s;
    p.stateFrame = 0;
}

// Knockback. Implements the standard Smash knockback relationship, documented
// publicly by the competitive community for two decades:
//
//   kb = ((p/10 + p*d/20) * (200/(w+100)) * 1.4 + 18) * (s/100) + b
//
//   p = damage percent AFTER this hit    d = move damage
//   w = target weight (100 neutral)      s = knockback growth    b = base knockback
//
// Every term earns its place in how the game feels:
//   - p appears twice (alone and multiplied by d), so launch distance grows
//     superlinearly with damage. That's why matches escalate.
//   - 200/(w+100) makes heavy characters genuinely resist launch.
//   - the floor term guarantees even a weak jab moves someone.
//   - b vs s separates a reliable kill move (high base) from a combo starter that
//     only kills at high percent (high growth).
// `moveDamage` is passed in rather than read from `atk` because a charged smash
// deals more than its table value -- see resolveAttack.
fx computeKnockback(fx targetDamageBefore, const AttackData &atk, fx moveDamage, fx weight) {
    const Knockback &k = kKnockback;
    const fx p = targetDamageBefore + moveDamage;
    const fx d = moveDamage;

    const fx damageTerm = fx_div(p, k.damageDivisor) + fx_div(fx_mul(p, d), k.damageProductDivisor);
    const fx weightTerm = fx_div(k.weightNumerator, weight + k.weightOffset);

    fx kb = fx_mul(fx_mul(damageTerm, weightTerm), k.scale) + k.floor;
    kb = fx_mul(kb, fx_div(atk.knockbackGrowth, k.growthDivisor)) + atk.baseKnockback;
    return kb;
}

// Hitstun proportional to knockback: a big hit buys the attacker frames to follow
// up. That's where combos come from.
uint16_t computeHitstun(fx knockback) {
    int32_t frames = fx_to_int(fx_mul(knockback, kHitstun.perKnockback));
    if (frames < kHitstun.minFrames) frames = kHitstun.minFrames;
    if (frames > kHitstun.maxFrames) frames = kHitstun.maxFrames;
    return static_cast<uint16_t>(frames);
}

// Hitlag: the contact freeze, mirroring ftCommon_CalcHitlag.
//
//     frames = damage * perDamage + base
//     crouching multiplies it (their kb_squat_mul / x1A0 equivalent)
//     clamped to [minFrames, maxFrames]
//
// Scaled by move damage rather than knockback, so a big slow move freezes longer
// than a fast weak one regardless of how far it launches. That is what couples the
// visual weight of a hit to its commitment.
uint8_t computeHitlag(fx moveDamage, bool crouching) {
    fx frames = fx_mul(moveDamage, kHitlag.perDamage) + kHitlag.base;
    if (crouching) frames = fx_mul(frames, kHitlag.crouchMult);

    int32_t f = fx_to_int(frames);
    if (f < kHitlag.minFrames) f = kHitlag.minFrames;
    if (f > kHitlag.maxFrames) f = kHitlag.maxFrames;
    return static_cast<uint8_t>(f);
}

// SDI: shift POSITION during hitlag, mirroring ftCo_Damage_OnEveryHitlag.
//
// Runs on every frozen frame, gates on the stick VECTOR magnitude (not per-axis),
// accepts a fresh flick on EITHER axis, and adds straight to position rather than
// to velocity -- which is why it reads as an instant displacement instead of drift.
//
// On success both stick timers saturate, so one flick buys exactly one nudge.
// Multi-SDI therefore requires genuinely re-flicking mid-freeze; holding a
// direction gets you nothing, which is what makes it a technique.
void applySDI(Player &p, const Input &in) {
    if (p.sdiNudges >= kSDI.maxNudgesPerHitlag) return;

    const fx sx = fx_ratio(in.stickX, config::kStickRange);
    const fx sy = fx_ratio(in.stickY, config::kStickRange);

    // Vector magnitude, compared squared to avoid a needless sqrt. A diagonal
    // qualifies even when neither axis alone clears the threshold.
    const int64_t magSq = fx_len_sq(sx, sy);
    if (magSq < fx_sq(kSDI.minStickMag)) return;

    // Either axis freshly crossed is enough.
    const bool freshX = p.stickHeldX < kSDI.stickWindow;
    const bool freshY = p.stickHeldY < kSDI.stickWindow;
    if (!freshX && !freshY) return;

    p.x += fx_mul(sx, kSDI.posScale);
    p.y += fx_mul(sy, kSDI.posScale);
    p.sdiNudges++;

    // Saturate BOTH timers: one flick, one nudge.
    p.stickHeldX = kSmash.stickTimerMax;
    p.stickHeldY = kSmash.stickTimerMax;
}

// Decay knockback velocity along its own vector, independent of self-velocity.
// Below the snap threshold it's zeroed so control returns cleanly rather than
// bleeding out over a long vanishing tail.
void decayKnockback(Player &p, bool grounded) {
    if (p.kbVelX == 0 && p.kbVelY == 0) return;

    const fx amount = grounded ? kKnockback.groundDecay : kKnockback.airDecay;
    const fx mag = fx_sqrt(fx_mul(p.kbVelX, p.kbVelX) + fx_mul(p.kbVelY, p.kbVelY));

    if (mag <= kKnockback.snapToZero || mag <= amount) {
        p.kbVelX = 0;
        p.kbVelY = 0;
        return;
    }
    // Scale the vector down by (mag - amount)/mag: preserves direction exactly.
    const fx scale = fx_div(mag - amount, mag);
    p.kbVelX = fx_mul(p.kbVelX, scale);
    p.kbVelY = fx_mul(p.kbVelY, scale);
}

// Directional influence: holding perpendicular to your launch bends the
// trajectory, capped. Makes surviving a hit an active skill, not a dice roll.
int applyDI(int angleDeg, const Input &in, int8_t attackerFacing) {
    if (in.stickX == 0 && in.stickY == 0) return angleDeg;

    const fx sx = fx_ratio(in.stickX, config::kStickRange);
    const fx sy = fx_ratio(in.stickY, config::kStickRange);
    const fx mag = fx_sqrt(fx_mul(sx, sx) + fx_mul(sy, sy));
    if (mag < fx_ratio(kStick.deadzone, config::kStickRange)) return angleDeg;

    const int worldAngle = attackerFacing > 0 ? angleDeg : 180 - angleDeg;
    const fx launchX = fx_cos_deg(worldAngle);
    const fx launchY = -fx_sin_deg(worldAngle); // negate: +Y is down

    const fx nx = fx_div(sx, mag);
    const fx ny = fx_div(sy, mag);

    // 2D cross product: signed perpendicular offset in [-1, 1].
    const fx cross = fx_mul(launchX, ny) - fx_mul(launchY, nx);
    return angleDeg + fx_to_int(fx_mul(cross, fxi(kDI.maxBendDegrees)));
}

// Ground friction, two-tier and speed-dependent -- mirrors ft_80084F3C:
//
//     friction = base
//     if |vel| > walkSpeed:  friction *= fastMultiplier
//
// Fast motion brakes hard so a dash does not coast like ice; slow motion keeps the
// low base value so a wavedash retains its long glide. A single flat value cannot
// deliver both -- see the note in GroundMove.
// Friction with an explicit value, for states that brake harder than normal
// (run-brake). Kept separate so the two-tier speed rule still applies underneath.
void applyGroundFrictionScaled(Player &p, fx amount) {
    const Fighter &F = fighterOf(p);
    const fx speed = fx_abs(p.selfVelX);
    const fx friction =
        (speed > F.ground.walkSpeed) ? fx_mul(amount, F.ground.fastMultiplier) : amount;
    p.selfVelX = fx_decay(p.selfVelX, friction);
}

void applyGroundFriction(Player &p) {
    const Fighter &F = fighterOf(p);
    const fx speed = fx_abs(p.selfVelX);
    const fx friction = (speed > F.ground.walkSpeed)
                            ? fx_mul(F.ground.friction, F.ground.fastMultiplier)
                            : F.ground.friction;
    p.selfVelX = fx_decay(p.selfVelX, friction);
}

// --- Grab ------------------------------------------------------------------

// Grab hold duration, from the VICTIM's damage. A worn-down opponent is held far
// longer, which interacts with the arena's full-heal-on-KO: you are hardest to
// grab-punish right after scoring.
uint16_t computeGrabHold(const Player &victim, const config::Grab &G) {
    int32_t frames = G.holdBaseFrames + fx_to_int(fx_mul(victim.damage, G.holdPerDamage));
    if (frames > G.holdMaxFrames) frames = G.holdMaxFrames;
    if (frames < 1) frames = 1;
    return static_cast<uint16_t>(frames);
}

// Link a grabber and victim. Two-way references, so the resolution pass can find
// the pair from either end.
void linkGrab(GameState &gs, int grabberIdx, int victimIdx) {
    Player &g = gs.players[grabberIdx];
    Player &v = gs.players[victimIdx];
    const config::Grab &G = fighterOf(g).grab;

    g.grabPartner = static_cast<uint8_t>(victimIdx);
    g.isGrabber = true;
    g.grabHoldFrames = computeGrabHold(v, G);
    g.attackId = ATK_NONE;
    enterState(g, ActionState::GrabHold);

    v.grabPartner = static_cast<uint8_t>(grabberIdx);
    v.isGrabber = false;
    v.grabHoldFrames = g.grabHoldFrames;
    v.selfVelX = 0;
    v.selfVelY = 0;
    v.kbVelX = 0;
    v.kbVelY = 0;
    v.hitstunFrames = 0;
    v.attackId = ATK_NONE;
    v.throwDir = 0;
    enterState(v, ActionState::Grabbed);
}

// Sever a grab, pushing both players apart so a break resets neutral.
void breakGrab(GameState &gs, int idx) {
    Player &p = gs.players[idx];
    if (p.grabPartner >= kMaxPlayers) return;

    Player &other = gs.players[p.grabPartner];
    const Player &grabber = p.isGrabber ? p : other;
    const config::Grab &G = fighterOf(grabber).grab;

    const int8_t gFacing = grabber.facing;
    Player &gp = p.isGrabber ? p : other;
    Player &vp = p.isGrabber ? other : p;

    gp.selfVelX = -G.releasePushX * gFacing;
    vp.selfVelX = G.releasePushX * gFacing;

    Player *pair[2] = {&gp, &vp};
    for (Player *q : pair) {
        q->grabPartner = kNoAttacker;
        q->isGrabber = false;
        q->grabHoldFrames = 0;
        q->throwDir = 0;
        q->throwFrames = 0;
        q->groundActionFrames = static_cast<uint8_t>(G.releaseFrames);
    }
    enterState(gp, ActionState::GrabRelease);
    enterState(vp, ActionState::GrabRelease);
}

// Start a throw. Direction is a RISING-EDGE flick after the grab connects -- you
// cannot hold a direction into a grab.
//
// Duration scales with the VICTIM's weight (their x37C), which is the opposite of
// what you might expect: a heavy character is not harder to throw, just slower to
// throw. Knockback uses a global weight constant instead, so throw distance does
// not depend on who you threw.
void startThrow(GameState &gs, int grabberIdx, uint8_t dir) {
    Player &g = gs.players[grabberIdx];
    if (g.grabPartner >= kMaxPlayers) return;
    Player &v = gs.players[g.grabPartner];

    const fx weightFactor = kFxOne + fx_mul(fighterOf(v).body.weight, kThrow.weightDurationScale);
    int32_t total = fx_to_int(fx_mul(fxi(kThrow.totalFrames), weightFactor));
    if (total < kThrow.windupFrames + 1) total = kThrow.windupFrames + 1;

    g.throwDir = dir;
    g.throwFrames = static_cast<uint8_t>(total);
    enterState(g, ActionState::Throwing);

    v.throwDir = dir;
    v.throwFrames = static_cast<uint8_t>(total);
    // The escape budget is ZEROED here, exactly as theirs is: once a throw starts
    // it is guaranteed. All escaping happens during the hold.
    v.grabHoldFrames = 0;
    enterState(v, ActionState::Thrown);
}

// Release the victim mid-throw with knockback.
void releaseThrow(GameState &gs, int grabberIdx) {
    Player &g = gs.players[grabberIdx];
    if (g.grabPartner >= kMaxPlayers) return;
    Player &v = gs.players[g.grabPartner];

    fx damage = kThrow.damageF, growth = kThrow.growthF;
    int angle = kThrow.angleF;
    switch (g.throwDir) {
    case 2:
        damage = kThrow.damageB;
        growth = kThrow.growthB;
        angle = kThrow.angleB;
        break;
    case 3:
        damage = kThrow.damageU;
        growth = kThrow.growthU;
        angle = kThrow.angleU;
        break;
    case 4:
        damage = kThrow.damageD;
        growth = kThrow.growthD;
        angle = kThrow.angleD;
        break;
    default: break;
    }

    // Knockback uses the GLOBAL weight constant, not the victim's weight. That is
    // deliberate in the original -- throw distance is the same whoever you threw.
    AttackData throwBox{};
    throwBox.damage = damage;
    throwBox.baseKnockback = kThrow.baseKB;
    throwBox.knockbackGrowth = growth;
    throwBox.angleDeg = angle;

    const fx kb = computeKnockback(v.damage, throwBox, damage, kThrow.weightConstant);
    v.damage += damage;

    const int worldAngle = g.facing > 0 ? angle : 180 - angle;
    const fx speed = fx_mul(kb, kKnockback.velocityScale);
    v.kbVelX = fx_mul(fx_cos_deg(worldAngle), speed);
    v.kbVelY = -fx_mul(fx_sin_deg(worldAngle), speed);
    v.selfVelX = 0;
    v.selfVelY = 0;
    v.hitstunFrames = computeHitstun(kb);
    v.lastKilledBy = static_cast<uint8_t>(grabberIdx);
    enterState(v, ActionState::Hitstun);

    // Unlink: the victim is airborne and on their own now.
    v.grabPartner = kNoAttacker;
    v.throwDir = 0;
    v.throwFrames = 0;
    g.grabPartner = kNoAttacker;
    g.isGrabber = false;
}

// Grab attempt resolution -- a hitbox with a catch element, reusing the same
// closest-point overlap test as attacks. Nearest target wins, as theirs does.
void resolveGrab(GameState &gs, int grabberIdx) {
    Player &a = gs.players[grabberIdx];
    const config::Grab &G = fighterOf(a).grab;

    if (a.stateFrame < static_cast<uint16_t>(G.startupFrames)) return;
    if (a.stateFrame >= static_cast<uint16_t>(G.startupFrames + G.activeFrames)) return;

    const fx hbX = a.x + G.reachX * a.facing;
    const fx hbY = a.y + G.reachY;

    int best = -1;
    int64_t bestDist = 0;
    for (int t = 0; t < kMaxPlayers; ++t) {
        if (t == grabberIdx) continue;
        Player &d = gs.players[t];
        if (!d.active || d.state == ActionState::Dead) continue;
        if (d.invulnFrames > 0) continue;
        // Already in a grab, either side -- cannot be re-grabbed.
        if (d.grabPartner < kMaxPlayers) continue;
        // Airborne opponents cannot be grabbed: ground-only, as ours are.
        if (d.y < gs.stage.groundY - fxi(4)) continue;

        const config::Body &dBody = fighterOf(d).body;
        const fx left = d.x - dBody.halfWidth, right = d.x + dBody.halfWidth;
        const fx top = d.y - dBody.height, bottom = d.y;
        const fx dx = hbX - fx_clamp(hbX, left, right);
        const fx dy = hbY - fx_clamp(hbY, top, bottom);
        const int64_t distSq = fx_len_sq(dx, dy);
        if (distSq > fx_sq(G.radius)) continue;

        if (best < 0 || distSq < bestDist) {
            best = t;
            bestDist = distSq;
        }
    }

    if (best >= 0) linkGrab(gs, grabberIdx, best);
}

// Mash drains, matching their two INDEPENDENT inputs per frame: any fresh button,
// and any change in stick direction. Both can fire together, which is why circling
// the stick is the optimal escape.
fx grabMashDrain(const Input &in, const Input &prev, const config::Grab &G) {
    fx drain = G.drainPerFrame;
    if (in.pressed(BtnJump, prev) || in.pressed(BtnAttack, prev) || in.pressed(BtnShield, prev)) {
        drain += G.drainPerMash;
    }
    const bool stickMoved = (in.stickX > kStick.deadzone) != (prev.stickX > kStick.deadzone) ||
                            (in.stickX < -kStick.deadzone) != (prev.stickX < -kStick.deadzone) ||
                            (in.stickY > kStick.deadzone) != (prev.stickY > kStick.deadzone) ||
                            (in.stickY < -kStick.deadzone) != (prev.stickY < -kStick.deadzone);
    if (stickMoved) drain += G.drainPerMash;
    return drain;
}

// Throw direction from a rising-edge flick. 0 means no throw requested.
uint8_t chooseThrowDir(const Player &p, const Input &in) {
    const int8_t t = kSmash.deflection;
    if (in.stickY <= -t && p.stickHeldY < kSmash.flickWindow) return 3; // up
    if (in.stickY >= t && p.stickHeldY < kSmash.flickWindow) return 4;  // down
    if (p.stickHeldX < kSmash.flickWindow) {
        if (in.stickX >= t) return (p.facing > 0) ? 1 : 2;
        if (in.stickX <= -t) return (p.facing > 0) ? 2 : 1;
    }
    return 0;
}

// --- Shield ----------------------------------------------------------------

// Shield health lazily initialises to full on first use, so Player stays POD with
// no constructor -- a requirement for the memcpy-based rollback snapshots.
void ensureShieldInit(Player &p) {
    if (!p.shieldInit) {
        p.shieldHealth = fighterOf(p).shield.maxHealth;
        p.shieldInit = true;
    }
}

bool isShieldState(const Player &p) {
    return p.state == ActionState::ShieldOn || p.state == ActionState::Shield ||
           p.state == ActionState::ShieldStun;
}

// States where the shield must NOT regenerate. Distinct from isShieldState: a
// broken shield is not "active", but refilling it while the victim is still being
// punished would erase the entire cost of breaking it.
bool blocksShieldRegen(const Player &p) {
    return isShieldState(p) || p.state == ActionState::ShieldBroken ||
           p.state == ActionState::Dizzy;
}

// Regenerate whenever the shield is NOT active. Runs during release lag too, which
// is what makes shield-pressure sustainable rather than a one-shot resource.
void regenShield(Player &p) {
    const config::Shield &S = fighterOf(p).shield;
    ensureShieldInit(p);
    if (blocksShieldRegen(p)) return;
    if (p.shieldHealth < S.maxHealth) {
        p.shieldHealth = fx_min(p.shieldHealth + S.regenPerFrame, S.maxHealth);
    }
}

// Break the shield: launched helpless, then dizzy. Dizzy length SHRINKS with
// damage, so a break at low percent is a far longer punish than one at high --
// which stops the mechanic from being a free kill late in a stock.
void breakShield(Player &p) {
    const Fighter &F = fighterOf(p);
    p.shieldHealth = 0;
    p.selfVelY = kShieldBreak.launchVelY;
    p.selfVelX = 0;

    int32_t dizzy =
        kShieldBreak.dizzyBase + fx_to_int(fx_mul(p.damage, kShieldBreak.dizzyPerDamage));
    if (dizzy < kShieldBreak.dizzyMin) dizzy = kShieldBreak.dizzyMin;
    p.dizzyFrames = static_cast<uint16_t>(dizzy);

    p.shieldStunFrames = 0;
    p.shieldHoldFrames = 0;
    (void)F;
    enterState(p, ActionState::ShieldBroken);
}

// Apply a hit to a shield. Returns true if the shield absorbed it.
//
// Two accumulators, deliberately: shieldstun scales with the LARGEST single hit,
// health loss with the SUM. Collapsing them would make multi-hit moves wrong.
bool absorbOnShield(Player &d, Player &a, fx moveDamage, int8_t attackerFacing) {
    const config::Shield &S = fighterOf(d).shield;
    ensureShieldInit(d);

    // Health: proportional term plus a FLAT cost per hit, so many weak hits are
    // meaningfully worse for the shield than one big one.
    d.shieldDamageSum += moveDamage;
    if (moveDamage > d.shieldLargestHit) d.shieldLargestHit = moveDamage;
    d.shieldHealth -= fx_mul(moveDamage, S.damageScale) + S.damageFlat;

    if (d.shieldHealth <= 0) {
        breakShield(d);
        return true;
    }

    // Shieldstun from the largest hit.
    int32_t stun = fx_to_int(fx_mul(d.shieldLargestHit, S.stunScale) + S.stunFlat);
    if (stun < 1) stun = 1;
    if (stun > S.stunMaxFrames) stun = S.stunMaxFrames;
    d.shieldStunFrames = static_cast<uint8_t>(stun);

    // Pushback, LATCHED rather than applied now.
    //
    // Hitlag is set on this same frame, so the freeze runs first -- and the frozen
    // states call applyGroundFriction, which would eat a small velocity entirely
    // before it ever moved anyone (the two-tier friction is deliberately strong at
    // low speed). Latching and applying on the frame the freeze lifts is what makes
    // the push actually land.
    fx push = fx_mul(fxi(stun), S.pushbackScale);
    push = fx_min(push, S.pushbackCap);
    d.pendingPushX = push * (attackerFacing > 0 ? 1 : -1);

    // The ATTACKER is pushed too -- hitting a shield is a positional loss, not free
    // pressure. That cost is what makes shielding a real answer to offence.
    const fx aPush = fx_mul(moveDamage, S.attackerPushScale) + S.attackerPushFlat;
    a.pendingPushX = -aPush * attackerFacing;

    enterState(d, ActionState::ShieldStun);
    return true;
}

// --- Ground escapes --------------------------------------------------------

void startRoll(Player &p, int8_t dir) {
    const config::GroundEscape &E = fighterOf(p).escape;
    p.escapeDir = dir;
    p.rollStartX = p.x;
    p.selfVelX = 0;
    p.invulnFrames = 0; // granted once the invuln window opens, not immediately
    enterState(p, dir == p.facing ? ActionState::RollForward : ActionState::RollBack);
    (void)E;
}

void startSpotDodge(Player &p) {
    p.invulnFrames = 0;
    enterState(p, ActionState::SpotDodge);
}

// Escape inputs available out of shield. Spotdodge is checked BEFORE roll, matching
// their IASA ordering -- so a down-forward diagonal gives a spotdodge, not a roll.
// Returns true if an escape started.
bool checkEscapeInputs(Player &p, const Input &in) {
    // Down -> spotdodge. Requires a FRESH flick, same recency primitive as
    // smash-vs-tilt, fast-fall, and SDI.
    if (in.stickY >= kSmash.deflection && p.stickHeldY < kSmash.flickWindow) {
        startSpotDodge(p);
        return true;
    }
    // Sideways -> roll.
    if ((in.stickX >= kSmash.deflection || in.stickX <= -kSmash.deflection) &&
        p.stickHeldX < kSmash.flickWindow) {
        startRoll(p, in.stickX > 0 ? 1 : -1);
        return true;
    }
    return false;
}

void applyGravity(Player &p) {
    const Fighter &F = fighterOf(p);
    const fx cap = p.fastFalling ? F.air.fastFallSpeed : F.air.termVelocity;
    if (p.selfVelY < cap) { p.selfVelY = fx_min(p.selfVelY + F.air.gravity, cap); }
}

// Track how long each stick axis has been continuously deflected.
//
// The value RESETS TO 0 on the frame the stick crosses the threshold, then counts
// up while it stays deflected (saturating, so it never wraps). So a small value
// means "just flicked" and a large value means "been holding this".
//
// That single distinction is what separates a smash from a tilt, and it makes both
// input methods work through one code path -- see chooseGroundAttack.
// Mirrors the decomp's x670/x671_timer_lstick_tilt_x/y.
void updateStickTimers(Player &p, const Input &in, const Input &prev) {
    const int8_t t = kSmash.deflection;

    const bool xNow = (in.stickX >= t) || (in.stickX <= -t);
    const bool xPrevSameDir =
        (in.stickX >= t && prev.stickX >= t) || (in.stickX <= -t && prev.stickX <= -t);
    if (xNow) {
        if (xPrevSameDir) {
            if (p.stickHeldX < kSmash.stickTimerMax) p.stickHeldX++;
        } else {
            p.stickHeldX = 0; // fresh crossing -- this is the flick
        }
    } else {
        p.stickHeldX = kSmash.stickTimerMax; // saturated: not a flick
    }

    const bool yNow = (in.stickY >= t) || (in.stickY <= -t);
    const bool yPrevSameDir =
        (in.stickY >= t && prev.stickY >= t) || (in.stickY <= -t && prev.stickY <= -t);
    if (yNow) {
        if (yPrevSameDir) {
            if (p.stickHeldY < kSmash.stickTimerMax) p.stickHeldY++;
        } else {
            p.stickHeldY = 0;
        }
    } else {
        p.stickHeldY = kSmash.stickTimerMax;
    }
}

// Pick which ground attack comes out. This is the input matrix:
//
//   no direction              -> jab
//   direction, stale crossing -> tilt   (you were already holding it)
//   direction, fresh crossing -> smash  (you flicked it)
//
// Vertical wins ties: a diagonal flick reads as up/down rather than side, which
// makes up-smash reachable without needing a perfectly vertical input.
uint8_t chooseGroundAttack(const Player &p, const Input &in) {
    const int8_t t = kSmash.deflection;
    const bool up = in.stickY <= -t;
    const bool down = in.stickY >= t;
    const bool side = (in.stickX >= t) || (in.stickX <= -t);

    const bool yFlick = p.stickHeldY < kSmash.flickWindow;
    const bool xFlick = p.stickHeldX < kSmash.flickWindow;

    if (up) return yFlick ? ATK_SMASH_UP : ATK_TILT_UP;
    if (down) return yFlick ? ATK_SMASH_DOWN : ATK_TILT_DOWN;
    if (side) return xFlick ? ATK_SMASH_SIDE : ATK_TILT_SIDE;
    return ATK_JAB;
}

// Aerials are chosen by direction alone -- there is no air smash tier, so no
// flick check. Forward vs back is relative to FACING, which is why back air can be
// stronger: you have to turn around or preserve momentum to land it.
uint8_t chooseAirAttack(const Player &p, const Input &in) {
    const int8_t t = kSmash.deflection;
    if (in.stickY <= -t) return ATK_AIR_UP;
    if (in.stickY >= t) return ATK_AIR_DOWN;
    if (in.stickX >= t) return (p.facing > 0) ? ATK_AIR_FORWARD : ATK_AIR_BACK;
    if (in.stickX <= -t) return (p.facing > 0) ? ATK_AIR_BACK : ATK_AIR_FORWARD;
    return ATK_AIR_NEUTRAL;
}

void startAttack(Player &p, uint8_t attackId, bool airborne) {
    enterState(p, airborne ? ActionState::AttackAir : ActionState::AttackGround);
    p.attackId = attackId;
    p.attackFrame = 0;
    p.attackConnected = false;
    p.charging = false;
    p.chargeFrames = 0;

    const AttackData &atk = attackOf(p, attackId);
    if (atk.selfVelY != 0) p.selfVelY = atk.selfVelY;
    if (atk.selfVelX != 0) p.selfVelX = atk.selfVelX * p.facing;
}

// Enter a dash, applying the initial impulse.
//
// The impulse is VELOCITY-RELATIVE, from ftCo_Dash_Enter: if you are already
// moving against your new facing you get the full dashInitVel, otherwise only the
// difference up to it. That is why a dash reversal snaps but continuing a dash
// stays smooth -- one rule, two feels.
void enterDash(Player &p, int8_t dir) {
    const Fighter &F = fighterOf(p);
    p.facing = dir;

    const fx target = F.ground.dashInitVel * dir;
    if ((p.selfVelX > 0) != (dir > 0) && p.selfVelX != 0) {
        p.selfVelX = target; // reversing: full impulse
    } else if (fx_abs(p.selfVelX) < fx_abs(target)) {
        p.selfVelX = target; // slower than the impulse: snap up
    }
    // Already faster than the impulse: keep the momentum, do not slow down.

    // Saturate the stick timer so the flick that STARTED this dash cannot
    // immediately re-trigger a fresh one. (They set 0xFE here for the same reason.)
    p.stickHeldX = kSmash.stickTimerMax;
    enterState(p, ActionState::Dash);
}

// Accelerate ground velocity toward a target, with an optional taper.
//
// Run tapers by (1 - vel/target): hard acceleration at low speed, easing into the
// cap. From ftCo_Run_Phys. Without it a run hits top speed abruptly and reads as a
// switch rather than a build-up.
void accelGround(Player &p, fx accel, fx target, bool taper) {
    const Fighter &F = fighterOf(p);

    if (taper && target != 0) {
        const fx frac = fx_div(p.selfVelX, target);
        if (frac > 0 && frac < kFxOne) {
            accel = fx_mul(accel, fx_mul(kFxOne - frac, F.ground.runTaper));
        }
    }

    if (target > 0) {
        p.selfVelX = fx_min(p.selfVelX + accel, fx_max(target, p.selfVelX));
    } else {
        p.selfVelX = fx_max(p.selfVelX - accel, fx_min(target, p.selfVelX));
    }
    p.selfVelX = fx_clamp(p.selfVelX, -F.ground.maxHorizontal, F.ground.maxHorizontal);
}

// Shared entry checks for every actionable grounded state: jump and attack.
// Returns true if a new action was started.
bool checkGroundActions(Player &p, const Input &in, const Input &prev) {
    // GRAB before shield: shield-held + attack-pressed is the grab input (Melee's Z
    // is literally that combo). Checked first because both conditions include a
    // held shield, so the shield branch would otherwise swallow every grab.
    if (in.held(BtnShield) && in.pressed(BtnAttack, prev) && p.grabPartner >= kMaxPlayers) {
        p.attackId = ATK_NONE;
        enterState(p, ActionState::Grabbing);
        return true;
    }

    // Shield next: it is the only genuinely free use of a grounded shield press.
    // (BtnShield already drives air dodge, L-cancel, tech, get-up buffering and
    // ledge roll -- all in states that are not actionable-grounded, so there is no
    // collision here.)
    if (in.held(BtnShield)) {
        ensureShieldInit(p);
        if (p.shieldHealth > 0) {
            p.shieldHoldFrames = 0;
            p.shieldLargestHit = 0;
            p.shieldDamageSum = 0;
            enterState(p, ActionState::ShieldOn);
            return true;
        }
    }
    if (in.pressed(BtnJump, prev)) {
        enterState(p, ActionState::Jumpsquat);
        p.jumpHeld = true;
        return true;
    }
    if (in.pressed(BtnAttack, prev)) {
        // Face the input before committing, so a flick-and-attack the other way
        // turns you around rather than swinging backwards.
        if (in.stickX >= kSmash.deflection)
            p.facing = 1;
        else if (in.stickX <= -kSmash.deflection)
            p.facing = -1;
        startAttack(p, chooseGroundAttack(p, in), false);
        return true;
    }
    return false;
}

// Idle / Walk. A fresh hard flick starts a dash; a gentler hold walks.
void handleGroundMovement(Player &p, const Input &in, const Input &prev) {
    const Fighter &F = fighterOf(p);
    if (checkGroundActions(p, in, prev)) return;

    const bool deflected = (in.stickX > kStick.deadzone || in.stickX < -kStick.deadzone);
    if (!deflected) {
        applyGroundFriction(p);
        if (p.state != ActionState::Idle) enterState(p, ActionState::Idle);
        return;
    }

    const int8_t dir = in.stickX > 0 ? 1 : -1;
    const bool hard = (in.stickX > kStick.hard || in.stickX < -kStick.hard);
    const bool freshFlick = p.stickHeldX < kSmash.flickWindow;

    // A hard FLICK dashes. A hard hold that is not fresh just walks at top walk
    // speed -- so you cannot get a dash by leaning on the stick, only by flicking.
    if (hard && freshFlick) {
        // Reversing on the spot costs turn frames rather than flipping for free.
        if (p.facing != dir && p.state != ActionState::Idle) {
            p.facing = dir;
            p.groundActionFrames = static_cast<uint8_t>(F.ground.turnFrames);
            enterState(p, ActionState::Turn);
            return;
        }
        enterDash(p, dir);
        return;
    }

    // Walking: ramp up rather than snapping. First frame gets walkInitVel.
    if (p.facing != dir) {
        p.facing = dir;
        p.selfVelX = F.ground.walkInitVel * dir;
    } else if (fx_abs(p.selfVelX) < fx_abs(F.ground.walkInitVel)) {
        p.selfVelX = F.ground.walkInitVel * dir;
    }
    accelGround(p, F.ground.walkAccel, F.ground.walkSpeed * dir, false);
    if (p.state != ActionState::Walk) enterState(p, ActionState::Walk);
}

// Dash: the interruptible burst. THIS is where dash-dancing lives.
//
// Mirrors ftCo_Dash_CheckInput -- a fresh stick flick either re-enters the dash
// (same direction) or turns (opposite). Because the re-entry is available every
// frame of the dash, flicking back and forth keeps producing fresh dashes. Run has
// no equivalent check, which is precisely why you cannot dance out of a run.
void handleDash(Player &p, const Input &in, const Input &prev) {
    const Fighter &F = fighterOf(p);
    if (checkGroundActions(p, in, prev)) return;

    const int8_t dir = in.stickX > 0 ? 1 : -1;
    const bool deflected = (in.stickX >= kSmash.deflection || in.stickX <= -kSmash.deflection);
    const bool freshFlick = p.stickHeldX < kSmash.flickWindow;

    if (deflected && freshFlick) {
        if (dir != p.facing) {
            // Dash-dance: reversing out of a dash is quicker than a standing turn.
            p.facing = dir;
            p.groundActionFrames = static_cast<uint8_t>(F.ground.dashTurnFrames);
            enterState(p, ActionState::Turn);
        } else {
            enterDash(p, dir); // re-dash, fresh impulse
        }
        return;
    }

    // Still holding the direction: accelerate toward dash speed.
    if (deflected && dir == p.facing) {
        accelGround(p, F.ground.dashAccel, F.ground.dashSpeed * dir, false);
        // The dash window expiring COMMITS you to a run -- no longer interruptible.
        if (p.stateFrame + 1 >= static_cast<uint16_t>(F.ground.dashFrames)) {
            enterState(p, ActionState::Run);
        }
        return;
    }

    // Released: brake out of the dash back to neutral.
    applyGroundFriction(p);
    if (p.selfVelX == 0) enterState(p, ActionState::Idle);
}

// Run: committed. No dash re-entry, so no dancing. Leaving requires RunBrake.
void handleRun(Player &p, const Input &in, const Input &prev) {
    const Fighter &F = fighterOf(p);
    if (checkGroundActions(p, in, prev)) return;

    const int8_t dir = in.stickX > 0 ? 1 : -1;
    const bool holding =
        (in.stickX >= kSmash.deflection || in.stickX <= -kSmash.deflection) && dir == p.facing;

    if (holding) {
        // Tapered acceleration -- see accelGround.
        accelGround(p, F.ground.runAccel, F.ground.dashSpeed * dir, true);
        return;
    }

    // Released or reversed: you must brake. That commitment is the cost of running.
    p.groundActionFrames = static_cast<uint8_t>(F.ground.runBrakeFrames);
    enterState(p, ActionState::RunBrake);
}

// Directional air dodge. The wavedash enabler: this sets velocity along the stick,
// and crucially nothing zeroes horizontal velocity on landing -- ground friction
// merely decays it. Aim shallow-and-down and you slide along the ground.
//
// Note that no function here is named "wavedash". The technique is emergent: it
// falls out of three independent rules (dodge sets velocity / landing zeroes only
// vertical velocity / friction decays horizontal). That's how Melee's tech
// actually arose -- nobody implemented it. Write orthogonal rules that preserve
// momentum and players will discover things you never designed.
void startAirDodge(Player &p, const Input &in) {
    const Fighter &F = fighterOf(p);
    enterState(p, ActionState::AirDodge);
    p.invulnFrames = static_cast<uint16_t>(F.airDodge.invulnFrames);

    // Dodging spends your remaining jumps (their ftCommon_UseAllJumps). Combined
    // with the helpless exit, this is what makes an air dodge a real commitment
    // rather than a free repositioning tool.
    if (F.airDodge.consumesJumps) { p.airJumps = static_cast<uint8_t>(F.jump.maxAirJumps); }

    const fx sx = fx_ratio(in.stickX, config::kStickRange);
    const fx sy = fx_ratio(in.stickY, config::kStickRange);
    const fx mag = fx_sqrt(fx_mul(sx, sx) + fx_mul(sy, sy));

    if (mag > fx_ratio(kStick.deadzone, config::kStickRange)) {
        p.selfVelX = fx_mul(fx_div(sx, mag), F.airDodge.speed);
        p.selfVelY = fx_mul(fx_div(sy, mag), F.airDodge.speed);
    } else {
        p.selfVelX = 0;
        p.selfVelY = 0;
    }
}

// Air drift, mirroring ftCommon_8007D28C:
//
//     accel = stick * stickMul + sign(stick) * base
//
// A stick-PROPORTIONAL term plus a flat floor. A light tilt drifts gently, a full
// deflection drifts hard. With a single constant, drift is all-or-nothing past the
// deadzone and the analog nuance is gone -- which matters most during hitstun,
// where fine drift control is how you survive a launch.
//
// Turning around is exempt from the target clamp (their `vel * accel < 0` case):
// pushing against your own momentum applies full acceleration, which is why
// reversals feel responsive rather than mushy. The hard ceiling still applies.
void applyAirDrift(Player &p, const Input &in) {
    const Fighter &F = fighterOf(p);

    if (in.stickX <= kStick.deadzone && in.stickX >= -kStick.deadzone) {
        p.selfVelX = fx_decay(p.selfVelX, F.air.friction);
        return;
    }

    const int8_t dir = in.stickX > 0 ? 1 : -1;
    const fx stick = fx_ratio(in.stickX, config::kStickRange);

    const fx accel = fx_mul(fx_abs(stick), F.air.stickMul) + F.air.acceleration;
    const fx target = fx_mul(stick, F.air.maxSpeed);

    const bool turningAround = (dir > 0) ? (p.selfVelX < 0) : (p.selfVelX > 0);

    if (dir > 0) {
        const fx next = p.selfVelX + accel;
        p.selfVelX = turningAround ? next : fx_min(next, fx_max(target, p.selfVelX));
    } else {
        const fx next = p.selfVelX - accel;
        p.selfVelX = turningAround ? next : fx_max(next, fx_min(target, p.selfVelX));
    }

    // Hard ceiling, separate from the drift target: carried momentum may exceed
    // what you could accelerate to, but never this.
    p.selfVelX = fx_clamp(p.selfVelX, -F.air.maxHorizontal, F.air.maxHorizontal);
}

void handleAirMovement(Player &p, const Input &in, const Input &prev) {
    const Fighter &F = fighterOf(p);
    if (in.pressed(BtnJump, prev) && p.airJumps < F.jump.maxAirJumps) {
        p.airJumps++;
        p.selfVelY = F.jump.airJumpVelocity;
        p.fastFalling = false;
        if (in.stickX > kStick.deadzone)
            p.facing = 1;
        else if (in.stickX < -kStick.deadzone)
            p.facing = -1;
        return;
    }

    // Fast fall: a fresh downward FLICK while already descending. Instant.
    //
    // The flick-recency check matters -- ftCommon_CheckFallFast gates on the same
    // stick timer that separates smashes from tilts. Without it, holding down
    // through a rise auto-fast-falls the instant you start descending, which makes
    // the mechanic fire when you did not ask for it.
    if (!p.fastFalling && p.selfVelY > 0 && in.stickY > kStick.hard &&
        p.stickHeldY < kSmash.fastFallFlickWindow) {
        p.fastFalling = true;
        p.selfVelY = F.air.fastFallSpeed;
        // Saturate so the same flick cannot re-trigger (they set 0xFE here too).
        p.stickHeldY = kSmash.stickTimerMax;
    }

    if (in.pressed(BtnShield, prev)) {
        startAirDodge(p, in);
        return;
    }
    if (in.pressed(BtnAttack, prev)) {
        startAttack(p, chooseAirAttack(p, in), true);
        return;
    }

    applyAirDrift(p, in);
}

// Resolve one attacker against all targets. Circle-vs-AABB overlap: cheap,
// stable, and precise enough that hits land where they look like they land.
void resolveAttack(GameState &gs, int attackerIdx, const Input inputs[kMaxPlayers]) {
    Player &a = gs.players[attackerIdx];
    if (a.attackId == ATK_NONE || a.attackId >= ATK_COUNT) return;
    const AttackData &atk = attackOf(a, a.attackId);

    if (a.attackFrame < static_cast<uint16_t>(atk.startup)) return;
    if (a.attackFrame >= static_cast<uint16_t>(atk.startup + atk.active)) return;
    if (a.attackConnected) return; // one hit per swing

    // Charge scaling. Both damage and knockback grow with charge, but damage grows
    // faster -- so a charged smash also raises the victim's percent, compounding
    // into the NEXT hit via the knockback formula. That's why fully-charged smashes
    // feel decisive rather than merely stronger.
    fx dmg = atk.damage;
    fx chargeKbMult = kFxOne;
    if (atk.chargeable && a.chargeFrames > 0) {
        const fx t = fx_div(fxi(a.chargeFrames), fxi(kSmash.maxChargeFrames));
        const fx tc = fx_clamp(t, 0, kFxOne);
        dmg = fx_mul(dmg, kFxOne + fx_mul(tc, kSmash.fullChargeDamageMult - kFxOne));
        chargeKbMult = kFxOne + fx_mul(tc, kSmash.fullChargeKnockbackMult - kFxOne);
    }

    const fx hbX = a.x + atk.reachX * a.facing;
    const fx hbY = a.y + atk.reachY;

    for (int t = 0; t < kMaxPlayers; ++t) {
        if (t == attackerIdx) continue;
        Player &d = gs.players[t];
        if (!d.active || d.state == ActionState::Dead) continue;
        if (d.invulnFrames > 0) continue;

        // Closest point on the defender's box to the hitbox center.
        //
        // The DEFENDER's own body defines their hurtbox -- a Bruiser is a bigger
        // target than a Scout. Using the attacker's body here (as a single global
        // fighter would force) would size every hurtbox by whoever was swinging.
        const config::Body &dBody = fighterOf(d).body;
        const fx left = d.x - dBody.halfWidth, right = d.x + dBody.halfWidth;
        const fx top = d.y - dBody.height, bottom = d.y;
        const fx dx = hbX - fx_clamp(hbX, left, right);
        const fx dy = hbY - fx_clamp(hbY, top, bottom);
        // 64-bit comparison: squaring a distance in 32-bit fixed point overflows
        // past ~181 units and wraps negative, which reads as a hit from across
        // the stage. See fx_len_sq in fixed.h.
        if (fx_len_sq(dx, dy) > fx_sq(atk.radius)) continue;

        // Shielded? The shield eats it: no damage, no knockback, no hitstun. Both
        // players get pushed apart instead, and the defender takes shieldstun.
        // Checked here -- after the overlap test but before anything is applied --
        // so a shielded hit never touches the damage or knockback pipeline.
        if (isShieldState(d)) {
            absorbOnShield(d, a, dmg, a.facing);
            const uint8_t lagS = computeHitlag(dmg, /*crouching*/ false);
            d.hitlagFrames = lagS;
            a.hitlagFrames = lagS;
            d.sdiNudges = 0;
            a.sdiNudges = 0;
            a.attackConnected = true;
            break;
        }

        // Charge-scaled damage feeds BOTH the knockback formula and the victim's
        // percent, so a charged smash launches further now and leaves them easier
        // to launch next time.
        // The DEFENDER's weight resists launch -- 200/(w+100) in the formula. This
        // is what makes a heavy character genuinely harder to kill.
        const fx kb = fx_mul(computeKnockback(d.damage, atk, dmg, dBody.weight), chargeKbMult);
        d.damage += dmg;

        const int angle = applyDI(atk.angleDeg, inputs[t], a.facing);
        const int worldAngle = a.facing > 0 ? angle : 180 - angle;
        const fx speed = fx_mul(kb, kKnockback.velocityScale);

        // Knockback lands in kbVel, NOT selfVel. That separation is what lets the
        // defender air-drift while being launched.
        d.kbVelX = fx_mul(fx_cos_deg(worldAngle), speed);
        d.kbVelY = -fx_mul(fx_sin_deg(worldAngle), speed);
        d.selfVelX = 0;
        d.selfVelY = 0;

        // Knocked off a ledge: release it and take the cooldown, exactly as the
        // decomp does in ftCo_Damage. Without this you could be hit off and
        // instantly regrab with fresh invulnerability.
        if (d.ledgeSide >= 0) {
            d.ledgeSide = -1;
            d.ledgeHangFrames = 0;
            d.ledgeCooldown = static_cast<uint8_t>(fighterOf(d).ledge.cooldownFrames);
        }

        // Hitlag freezes BOTH fighters. The defender's knockback is already set,
        // but nothing moves until the freeze expires -- so the launch reads as a
        // consequence of the impact rather than simultaneous with it.
        const uint8_t lag = computeHitlag(dmg, /*crouching*/ false);
        d.hitlagFrames = lag;
        d.sdiNudges = 0; // fresh budget per hit
        a.sdiNudges = 0;
        a.hitlagFrames =
            static_cast<uint8_t>(fx_to_int(fx_mul(fxi(lag), kHitlag.attackerFraction)));

        d.hitstunFrames = computeHitstun(kb);
        enterState(d, ActionState::Hitstun);
        d.fastFalling = false;
        d.lastKilledBy = static_cast<uint8_t>(attackerIdx);

        a.attackConnected = true;
        break; // single-target for MVP; multi-hit needs a per-attack hit list
    }
}

// Ledge grab detection.
//
// A ledge is catchable when the player is airborne, NOT rising, on the outward
// side of the edge, and inside the grab box. The "not rising" and "outward side"
// conditions together are what make recovery feel fair rather than magnetic: you
// cannot snap to a ledge while jumping up past it from below, and you cannot catch
// it while standing on the stage.
//
// Returns the ledge side (0 left, 1 right) or -1 for none.
int8_t detectLedgeGrab(const GameState &gs, const Player &p, const Ledge &L) {
    // Cooldown blocks every regrab -- the anti-stalling rule.
    if (p.ledgeCooldown > 0) return -1;
    // Rising past a ledge must not catch it.
    if (p.totalVelY() < 0) return -1;

    const Stage &s = gs.stage;

    struct Candidate {
        fx x;
        int8_t side;
    };
    const Candidate cands[2] = {{s.platformLeft, 0}, {s.platformRight, 1}};

    for (const Candidate &c : cands) {
        // Outward side only: left ledge is caught from the left of it, right ledge
        // from the right. Otherwise you could grab through the stage.
        const bool outward = (c.side == 0) ? (p.x <= c.x) : (p.x >= c.x);
        if (!outward) continue;

        const fx dx = fx_abs(p.x - c.x);
        if (dx > L.grabReachX) continue;

        // Facing requirement. You must be facing the LEDGE, which means facing
        // toward the stage: at the left ledge that is facing right (+1), at the
        // right ledge facing left (-1).
        //
        // This is what makes recovery deliberate rather than magnetic. Drifting
        // backwards past the edge does not save you -- you have to turn around
        // first, and that costs time you may not have.
        if (L.requireFacing) {
            const int8_t needFacing = (c.side == 0) ? 1 : -1;
            if (p.facing != needFacing) continue;
        }

        // Vertical window is asymmetric: mostly below the lip, with a little above,
        // because you approach a ledge from underneath when recovering.
        const fx dy = p.y - s.groundY; // +ve means below the stage surface
        if (dy < -L.grabReachUp) continue;
        if (dy > L.grabReachDown) continue;

        return c.side;
    }
    return -1;
}

// Snap a player onto a ledge and start hanging.
void attachToLedge(GameState &gs, Player &p, int8_t side, const Ledge &L) {
    const Stage &s = gs.stage;
    const fx edgeX = (side == 0) ? s.platformLeft : s.platformRight;

    p.ledgeSide = side;
    // Face the stage -- you are hanging with your back to open air.
    p.facing = (side == 0) ? 1 : -1;
    p.x = edgeX + ((side == 0) ? -L.hangOffsetX : L.hangOffsetX);
    p.y = s.groundY + L.hangOffsetY;

    p.selfVelX = 0;
    p.selfVelY = 0;
    p.kbVelX = 0;
    p.kbVelY = 0;
    p.fastFalling = false;
    p.hitstunFrames = 0;
    p.airJumps = 0; // regain your air jump: the ledge is a reset
    p.bounceCount = 0;
    p.attackId = ATK_NONE;
    p.ledgeHangFrames = static_cast<uint16_t>(L.hangFrames);
    p.invulnFrames = static_cast<uint16_t>(L.grabInvulnFrames);
    enterState(p, ActionState::LedgeHang);
}

// Release the ledge. EVERY exit routes through here so the cooldown can never be
// forgotten -- that single guarantee is what prevents infinite ledge camping.
void releaseLedge(Player &p, const Ledge &L) {
    p.ledgeSide = -1;
    p.ledgeHangFrames = 0;
    p.ledgeCooldown = static_cast<uint8_t>(L.cooldownFrames);
}

// Slow vs quick: one damage comparison, exactly as the decomp does it. Being badly
// damaged makes recovery itself more punishable, so pressure compounds when you can
// least afford it.
bool useSlowLedgeOption(const Player &p, const Ledge &L) { return p.damage >= L.slowThreshold; }

// Blast zones. Losing your last stock here is what gets you EJECTED from the
// stage -- the arena detects that by watching for `active` going false, and routes
// you to a different stage.
void checkBlastZones(GameState &gs, int idx) {
    Player &p = gs.players[idx];
    const Stage &s = gs.stage;
    if (p.x >= s.blastLeft && p.x <= s.blastRight && p.y >= s.blastTop && p.y <= s.blastBottom) {
        return;
    }

    p.stocks--;
    // Release any grab this player is part of. A dangling partner reference would
    // leave the other player permanently stuck holding a corpse.
    if (p.grabPartner < kMaxPlayers) {
        Player &other = gs.players[p.grabPartner];
        if (other.grabPartner == static_cast<uint8_t>(idx)) {
            other.grabPartner = kNoAttacker;
            other.isGrabber = false;
            other.grabHoldFrames = 0;
            other.throwDir = 0;
            other.throwFrames = 0;
            if (other.state == ActionState::GrabHold || other.state == ActionState::Pummel ||
                other.state == ActionState::Throwing || other.state == ActionState::Grabbed ||
                other.state == ActionState::Thrown) {
                enterState(other, ActionState::Idle);
            }
        }
        p.grabPartner = kNoAttacker;
        p.isGrabber = false;
        p.grabHoldFrames = 0;
    }
    enterState(p, ActionState::Dead);
    p.respawnTimer = static_cast<uint16_t>(kRespawn.waitFrames);
    p.selfVelX = p.selfVelY = 0;
    p.kbVelX = p.kbVelY = 0;
    p.damage = 0;
    p.fastFalling = false;
    p.hitstunFrames = 0;

    // Credit the KO. FULL HEAL is the key mechanic here: landing a KO wipes the
    // killer's damage back to 0%, so a strong player can hold a stage against a
    // stream of arrivals -- but only for as long as they keep converting. Take
    // damage without scoring and you become launchable like everyone else.
    if (p.lastKilledBy < kMaxPlayers && gs.players[p.lastKilledBy].active) {
        Player &killer = gs.players[p.lastKilledBy];
        if (killer.pendingKOs < kMaxPendingKOs) killer.pendingKOs++;
        killer.damage = 0;
    }
    p.lastKilledBy = kNoAttacker;
}

// Drive every grabbed victim's position from their grabber.
//
// This runs as its OWN pass after the per-player loop, and that is deliberate. Every
// other cross-player interaction in this simulation is a single-frame hitbox test:
// attacker reads defender, applies knockback, done. A grab hold is different -- it
// spans many frames during which the victim has no physics of their own and their
// position is a function of someone else's.
//
// Doing it inside the per-player loop would make the result depend on player index:
// a victim in a lower slot than their grabber would be positioned from the grabber's
// PREVIOUS frame, one in a higher slot from the current frame. We already hit
// exactly that class of bug with the hitlag countdown, where freeze length silently
// depended on slot order. A separate pass means both members of the pair see a
// consistent view, whichever slots they occupy.
void resolveGrabPairs(GameState &gs) {
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player &v = gs.players[i];
        if (!v.active) continue;
        if (v.isGrabber) continue;
        if (v.grabPartner >= kMaxPlayers) continue;
        if (v.state != ActionState::Grabbed && v.state != ActionState::Thrown) { continue; }

        Player &g = gs.players[v.grabPartner];
        if (!g.active || g.grabPartner != static_cast<uint8_t>(i)) {
            // Partner vanished (KO'd, disconnected). Release rather than leaving a
            // dangling reference -- a stale partner index would desync.
            v.grabPartner = kNoAttacker;
            v.grabHoldFrames = 0;
            v.throwDir = 0;
            v.throwFrames = 0;
            enterState(v, ActionState::Airborne);
            continue;
        }

        // Melee parents the victim's skeleton to a bone on the grabber. A plain
        // offset is the same mechanic without the rig.
        const config::Grab &G = fighterOf(g).grab;
        v.x = g.x + G.holdOffsetX * g.facing;
        v.y = g.y + G.holdOffsetY;
        v.facing = static_cast<int8_t>(-g.facing); // face your captor
        v.selfVelX = 0;
        v.selfVelY = 0;
        v.kbVelX = 0;
        v.kbVelY = 0;
    }
}

} // namespace

const Fighter &defaultFighter() { return config::kFighters[config::CHAR_SCOUT]; }

void step(GameState &gs, const Input inputs[kMaxPlayers], const Input prevInputs[kMaxPlayers]) {
    gs.tick++;

    // Hitlag countdown, done for EVERYONE before anyone acts. Kept out of the main
    // loop so freeze length cannot depend on player index -- see the freeze check
    // below for why that matters. A timer set this frame is untouched until next.
    for (Player &p : gs.players) {
        if (p.active && p.hitlagFrames > 0) p.hitlagFrames--;
    }

    for (int i = 0; i < kMaxPlayers; ++i) {
        Player &p = gs.players[i];
        if (!p.active) continue;

        const Input &in = inputs[i];
        const Input &prev = prevInputs[i];

        const Fighter &F = fighterOf(p);

        // --- Hitlag freeze ---------------------------------------------------
        // Nothing advances while frozen: no movement, no state progression, no
        // attack frames, no timers.
        //
        // The COUNTDOWN happens in a separate pass before this loop (see above).
        // Decrementing here would make the freeze length depend on player index:
        // a hit resolved during player 0's turn sets both fighters' timers, and
        // player 1 would then immediately decrement their own in the same frame,
        // losing a frame purely for being a higher slot. Symmetric freezes matter
        // for fairness, and an index-dependent asymmetry is exactly the kind of
        // thing that looks fine locally and reads as unfair online.
        //
        // Stick timers ARE still updated, because the freeze is when a defender
        // buffers their escape input -- and it is the window SDI will read.
        if (p.hitlagFrames > 0) {
            // Stick timers update first so a flick landing THIS frame is visible to
            // SDI immediately -- the freeze is short, and a one-frame delay would
            // swallow most attempts.
            updateStickTimers(p, in, prev);
            applySDI(p, in);
            continue;
        }

        // Shield pushback latched during hitlag lands now, on the first unfrozen
        // frame -- see absorbOnShield for why it cannot be applied at hit time.
        if (p.pendingPushX != 0) {
            p.selfVelX = p.pendingPushX;
            p.pendingPushX = 0;
        }

        // Stick timers must update BEFORE any input is consumed this frame, since
        // attack selection reads them to tell a flick from a hold.
        updateStickTimers(p, in, prev);

        if (p.invulnFrames > 0) p.invulnFrames--;
        if (p.lcancelTimer > 0) p.lcancelTimer--;
        if (p.techLockout > 0) p.techLockout--;
        if (p.ledgeCooldown > 0) p.ledgeCooldown--;

        // Shield regenerates every frame it is NOT active -- including during the
        // release lag, which is what keeps shield pressure sustainable instead of
        // being a one-shot resource.
        regenShield(p);
        if (!isShieldState(p)) {
            p.shieldLargestHit = 0;
            p.shieldDamageSum = 0;
        }
        if (in.pressed(BtnShield, prev)) {
            p.lcancelTimer = static_cast<uint8_t>(F.landing.lcancelWindow);
        }

        // --- Respawn ---------------------------------------------------------
        if (p.state == ActionState::Dead) {
            if (p.respawnTimer > 0) {
                p.respawnTimer--;
                continue;
            }
            if (p.stocks <= 0) {
                p.active = false;
                continue;
            }
            p.x = (gs.stage.platformLeft + gs.stage.platformRight) / 2;
            p.y = gs.stage.groundY - kRespawn.spawnHeight;
            p.selfVelX = p.selfVelY = 0;
            p.kbVelX = p.kbVelY = 0;
            p.airJumps = 0;
            p.invulnFrames = static_cast<uint16_t>(kRespawn.invulnFrames);
            enterState(p, ActionState::Airborne);
            continue;
        }

        // --- Per-state logic -------------------------------------------------
        switch (p.state) {
        case ActionState::Hitstun:
            if (p.hitstunFrames > 0) p.hitstunFrames--;
            if (p.hitstunFrames == 0) enterState(p, ActionState::Airborne);

            // Tech attempt: pressing shield while launched opens a window. If
            // the ground arrives while it is open, the landing is teched. If
            // the window expires first, you're locked out -- which is what
            // makes mashing shield a losing strategy rather than a free out.
            if (in.pressed(BtnShield, prev) && p.techLockout == 0 && p.techWindow == 0) {
                p.techWindow = static_cast<uint8_t>(F.knockdown.techWindow);
            }
            if (p.techWindow > 0) {
                p.techWindow--;
                if (p.techWindow == 0) {
                    p.techLockout = static_cast<uint8_t>(F.knockdown.techLockoutFrames);
                }
            }

            applyGravity(p);
            // Air drift is allowed during hitstun -- selfVel responds to the
            // stick while kbVel decays independently. This is the interaction
            // the decomposition exists to make possible.
            applyAirDrift(p, in);
            break;

        case ActionState::Jumpsquat:
            // Releasing jump during jumpsquat yields a short hop; holding it
            // gives a full jump. A few frames to decide -- the tight window is
            // the whole skill expression.
            if (!in.held(BtnJump)) p.jumpHeld = false;
            if (p.stateFrame + 1 >= static_cast<uint16_t>(F.jump.jumpsquatFrames)) {
                p.selfVelY = p.jumpHeld ? F.jump.fullVelocity : F.jump.hopVelocity;
                p.airJumps = 0;
                p.fastFalling = false;
                enterState(p, ActionState::Airborne);
            } else {
                applyGroundFriction(p);
            }
            break;

        case ActionState::AttackGround:
        case ActionState::AttackAir:    {
            const bool air = (p.state == ActionState::AttackAir);
            const AttackData &atk = attackOf(p, p.attackId);

            // Charge: a chargeable move HOLDS on its last pre-hitbox frame
            // while attack stays held, banking chargeFrames. Release (or cap
            // out) and the hitbox comes out with scaled damage/knockback.
            //
            // Freezing before startup rather than mid-swing is what makes the
            // charge readable: the opponent sees the wind-up held and knows a
            // committed hit is coming.
            const bool atChargePoint =
                atk.chargeable && p.attackFrame + 1 == static_cast<uint16_t>(atk.startup);

            if (atChargePoint && in.held(BtnAttack) && p.chargeFrames < kSmash.maxChargeFrames) {
                p.charging = true;
                p.chargeFrames++;
                applyGroundFriction(p);
                break; // hold this frame: do NOT advance attackFrame
            }
            p.charging = false;

            resolveAttack(gs, i, inputs);
            p.attackFrame++;
            if (air) {
                applyGravity(p);
                p.selfVelX = fx_decay(p.selfVelX, F.air.friction);
            } else {
                applyGroundFriction(p);
            }
            if (p.attackFrame >= static_cast<uint16_t>(atk.total)) {
                p.attackId = ATK_NONE;
                p.chargeFrames = 0;
                enterState(p, air ? ActionState::Airborne : ActionState::Idle);
            }
            break;
        }

        case ActionState::AirDodge:
            p.selfVelX = fx_decay(p.selfVelX, F.airDodge.momentumDecay);
            p.selfVelY = fx_decay(p.selfVelY, F.airDodge.momentumDecay);
            if (p.stateFrame >= static_cast<uint16_t>(F.airDodge.durationFrames)) {
                // Exits into HELPLESS, not plain Airborne. That is the whole
                // limiting mechanism -- FallHelpless has no air-dodge entry, so
                // one dodge per airtime falls out of the state graph rather than
                // needing a counter. (Melee: EscapeAir -> FallSpecial.)
                enterState(p, F.airDodge.oneUsePerAirtime ? ActionState::FallHelpless
                                                          : ActionState::Airborne);
                applyGravity(p);
            }
            break;

        case ActionState::FallHelpless:
            // Helpless: you may still DRIFT (and grab a ledge -- see the ledge
            // check below), but there is deliberately no jump, attack, or
            // air-dodge handling here. That absence is the mechanic.
            applyAirDrift(p, in);
            applyGravity(p);
            break;

        case ActionState::Landing: {
            // Uses the lag LATCHED at touchdown, never recomputed. The lcancel
            // window expires during the lag itself, so re-deciding here would
            // flip a successful L-cancel back to full lag partway through.
            applyGroundFriction(p);
            if (p.stateFrame + 1 >= static_cast<uint16_t>(p.landingLag)) {
                enterState(p, ActionState::Idle);
            }
            break;
        }

            // --- Knockdown family ------------------------------------------------

        case ActionState::Bounce:
            // A short airborne arc after slamming into the ground. Gravity
            // applies, so the ground-collision block below catches it again --
            // that second contact is what drops the player into DownWait.
            applyGravity(p);
            p.selfVelX = fx_decay(p.selfVelX, F.air.friction);
            break;

        case ActionState::DownWait: {
            // Lying down and vulnerable. Get-up options unlock after a short
            // delay so a knockdown always costs something, and a forced get-up
            // caps the maximum so nobody can lie there indefinitely.
            applyGroundFriction(p);

            // Buffer presses that arrive before options unlock, so mashing on
            // knockdown works instead of having the input eaten by the lockout.
            if (in.pressed(BtnJump, prev)) p.bufferedButtons |= BtnJump;
            if (in.pressed(BtnAttack, prev)) p.bufferedButtons |= BtnAttack;
            if (in.pressed(BtnShield, prev)) p.bufferedButtons |= BtnShield;

            const bool canAct =
                p.stateFrame >= static_cast<uint16_t>(F.knockdown.downWaitMinFrames);
            const bool forced =
                p.stateFrame >= static_cast<uint16_t>(F.knockdown.downWaitMaxFrames);

            if (canAct) {
                // Roll: stick left/right. Repositions and is invulnerable for
                // most of it, which is the safe escape.
                if (in.stickX >= kSmash.deflection || in.stickX <= -kSmash.deflection) {
                    p.rollDir = in.stickX > 0 ? 1 : -1;
                    p.selfVelX = F.knockdown.rollSpeed * p.rollDir;
                    p.invulnFrames = static_cast<uint16_t>(F.knockdown.rollInvulnFrames);
                    enterState(p, ActionState::GetUpRoll);
                    break;
                }
                // Get-up attack: covers both sides, but slow and weak. Punishes
                // someone standing over you rather than winning the exchange.
                if (p.bufferedButtons & BtnAttack) {
                    p.bufferedButtons = 0;
                    p.attackId = config::ATK_GETUP;
                    p.attackFrame = 0;
                    p.attackConnected = false;
                    enterState(p, ActionState::GetUpAttack);
                    break;
                }
                // Neutral get-up: jump or shield, briefly invulnerable.
                if (p.bufferedButtons & (BtnJump | BtnShield)) {
                    p.bufferedButtons = 0;
                    p.invulnFrames = static_cast<uint16_t>(F.knockdown.getUpInvulnFrames);
                    enterState(p, ActionState::GetUp);
                    break;
                }
            }
            if (forced) {
                p.invulnFrames = static_cast<uint16_t>(F.knockdown.getUpInvulnFrames);
                enterState(p, ActionState::GetUp);
            }
            break;
        }

        case ActionState::GetUp:
            applyGroundFriction(p);
            if (p.stateFrame + 1 >= static_cast<uint16_t>(F.knockdown.getUpFrames)) {
                p.bounceCount = 0;
                enterState(p, ActionState::Idle);
            }
            break;

        case ActionState::GetUpRoll:
            // Roll momentum decays across the roll so it covers a fixed-ish
            // distance rather than launching you across the stage.
            applyGroundFriction(p);
            if (p.stateFrame + 1 >= static_cast<uint16_t>(F.knockdown.rollFrames)) {
                p.bounceCount = 0;
                p.rollDir = 0;
                enterState(p, ActionState::Idle);
            }
            break;

        case ActionState::GetUpAttack: {
            resolveAttack(gs, i, inputs);
            p.attackFrame++;
            applyGroundFriction(p);
            if (p.attackFrame >= static_cast<uint16_t>(F.knockdown.getUpAttackFrames)) {
                p.attackId = ATK_NONE;
                p.bounceCount = 0;
                enterState(p, ActionState::Idle);
            }
            break;
        }

        case ActionState::Tech:
            // The reward for a well-timed tech: no knockdown, brief
            // invulnerability, back to neutral quickly. This is the mechanic
            // that makes combos escapable.
            applyGroundFriction(p);
            if (p.stateFrame + 1 >= static_cast<uint16_t>(F.knockdown.techFrames)) {
                p.bounceCount = 0;
                enterState(p, ActionState::Idle);
            }
            break;

            // --- Ledge family ----------------------------------------------------

        case ActionState::LedgeHang: {
            const Ledge &L = F.ledge;
            // Position is pinned: no gravity, no drift. Hanging is a hard stop.
            p.selfVelX = 0;
            p.selfVelY = 0;

            // Hang timeout -- the ledge is a refuge, not a hiding place.
            if (p.ledgeHangFrames > 0) p.ledgeHangFrames--;
            if (p.ledgeHangFrames == 0) {
                releaseLedge(p, L);
                enterState(p, ActionState::Airborne);
                break;
            }

            const bool slow = useSlowLedgeOption(p, L);

            // Drop off: hold down or away from the stage.
            const bool awayFromStage = (p.ledgeSide == 0) ? in.stickX <= -kSmash.deflection
                                                          : in.stickX >= kSmash.deflection;
            if (in.stickY >= kSmash.deflection || awayFromStage) {
                releaseLedge(p, L);
                enterState(p, ActionState::Airborne);
                break;
            }

            // Jump off the ledge.
            if (in.pressed(BtnJump, prev)) {
                releaseLedge(p, L);
                p.selfVelY = L.jumpVelY;
                p.selfVelX = (p.ledgeSide == 0) ? -L.jumpVelX : L.jumpVelX;
                p.ledgeActionFrames = static_cast<uint8_t>(L.jumpFrames);
                enterState(p, ActionState::LedgeJump);
                break;
            }

            // Ledge attack -- reaches onto the stage where an edgeguarder stands.
            if (in.pressed(BtnAttack, prev)) {
                releaseLedge(p, L);
                p.attackId = config::ATK_LEDGE;
                p.attackFrame = 0;
                p.attackConnected = false;
                p.ledgeActionFrames =
                    static_cast<uint8_t>(slow ? L.attackSlowFrames : L.attackQuickFrames);
                enterState(p, ActionState::LedgeAttack);
                break;
            }

            // Roll onto the stage -- invulnerable for most of it.
            if (in.pressed(BtnShield, prev)) {
                releaseLedge(p, L);
                p.ledgeActionFrames =
                    static_cast<uint8_t>(slow ? L.rollSlowFrames : L.rollQuickFrames);
                p.invulnFrames = static_cast<uint16_t>(L.rollInvulnFrames);
                enterState(p, ActionState::LedgeRoll);
                break;
            }

            // Climb up: hold toward the stage or up.
            const bool towardStage = (p.ledgeSide == 0) ? in.stickX >= kSmash.deflection
                                                        : in.stickX <= -kSmash.deflection;
            if (towardStage || in.stickY <= -kSmash.deflection) {
                releaseLedge(p, L);
                p.ledgeActionFrames =
                    static_cast<uint8_t>(slow ? L.climbSlowFrames : L.climbQuickFrames);
                enterState(p, ActionState::LedgeClimb);
                break;
            }
            break;
        }

        case ActionState::LedgeClimb:
            // Frame budget was LATCHED on entry, so slow/quick is decided once
            // rather than flipping mid-climb as damage changes.
            p.selfVelX = 0;
            p.selfVelY = 0;
            if (p.stateFrame + 1 >= static_cast<uint16_t>(p.ledgeActionFrames)) {
                // Placed just inside the ledge, standing on the stage.
                const Stage &st = gs.stage;
                const fx inward = F.body.halfWidth + fxi(2);
                p.x = (p.facing > 0) ? st.platformLeft + inward : st.platformRight - inward;
                p.y = st.groundY;
                enterState(p, ActionState::Idle);
            }
            break;

        case ActionState::LedgeRoll: {
            // Rolls past the edge onto the stage. Position is interpolated so it
            // ends at a fixed distance regardless of the frame count -- a slow
            // roll travels no further, it is just easier to punish.
            const Ledge &L = F.ledge;
            const Stage &st = gs.stage;
            const uint8_t total = p.ledgeActionFrames > 0 ? p.ledgeActionFrames : 1;
            const fx t = fx_div(fxi(p.stateFrame), fxi(total));
            const fx tc = fx_clamp(t, 0, kFxOne);

            const fx startX =
                (p.facing > 0) ? st.platformLeft - L.hangOffsetX : st.platformRight + L.hangOffsetX;
            const fx travel = fx_mul(L.rollDistance, tc);
            p.x = (p.facing > 0) ? startX + travel : startX - travel;
            p.y = st.groundY;
            p.selfVelX = 0;
            p.selfVelY = 0;

            if (p.stateFrame + 1 >= static_cast<uint16_t>(total)) {
                enterState(p, ActionState::Idle);
            }
            break;
        }

        case ActionState::LedgeAttack: {
            // Climbs up while swinging. Resolved like any other attack, so the
            // hitbox and knockback rules are shared rather than special-cased.
            const Stage &st = gs.stage;
            p.y = st.groundY;
            const fx inward = F.body.halfWidth + fxi(2);
            p.x = (p.facing > 0) ? st.platformLeft + inward : st.platformRight - inward;
            p.selfVelX = 0;
            p.selfVelY = 0;

            resolveAttack(gs, i, inputs);
            p.attackFrame++;
            if (p.attackFrame >= static_cast<uint16_t>(p.ledgeActionFrames)) {
                p.attackId = ATK_NONE;
                enterState(p, ActionState::Idle);
            }
            break;
        }

        case ActionState::LedgeJump:
            // A short scripted window, then normal airborne control resumes.
            applyGravity(p);
            if (p.stateFrame + 1 >= static_cast<uint16_t>(p.ledgeActionFrames)) {
                enterState(p, ActionState::Airborne);
            }
            break;

        case ActionState::Airborne:
            handleAirMovement(p, in, prev);
            applyGravity(p);
            break;

        case ActionState::Idle:
        case ActionState::Walk: handleGroundMovement(p, in, prev); break;

        case ActionState::Dash: handleDash(p, in, prev); break;

        case ActionState::Run:  handleRun(p, in, prev); break;

        case ActionState::RunBrake:
            // Committed stop. Attacks and jumps are allowed out of a brake --
            // that is the payoff for the commitment, and it is where dash-attack
            // and run-jump pressure come from.
            if (checkGroundActions(p, in, prev)) break;
            applyGroundFrictionScaled(p, F.ground.runBrakeFriction);
            if (p.groundActionFrames > 0) p.groundActionFrames--;
            if (p.groundActionFrames == 0 || p.selfVelX == 0) { enterState(p, ActionState::Idle); }
            break;

            // --- Grab family -----------------------------------------------------

        case ActionState::Grabbing: {
            // The attempt. Whiffing is a real punish window, which is the cost
            // that keeps grab from being a free answer to shielding.
            const config::Grab &G = F.grab;
            applyGroundFriction(p);
            resolveGrab(gs, i);
            if (p.state != ActionState::Grabbing) break; // connected
            if (p.stateFrame + 1 >= static_cast<uint16_t>(G.whiffFrames)) {
                enterState(p, ActionState::Idle);
            }
            break;
        }

        case ActionState::GrabHold: {
            const config::Grab &G = F.grab;
            applyGroundFriction(p);

            if (p.grabPartner >= kMaxPlayers) {
                enterState(p, ActionState::Idle);
                break;
            }

            // Throw: a rising-edge flick. Cannot be held into the grab.
            const uint8_t dir = chooseThrowDir(p, in);
            if (dir != 0) {
                startThrow(gs, i, dir);
                break;
            }

            // Pummel: does NOT reset the hold timer, so pummelling trades damage
            // against giving the victim more time to mash out.
            if (in.pressed(BtnAttack, prev)) {
                p.attackConnected = false; // so this pummel lands its damage
                enterState(p, ActionState::Pummel);
                break;
            }

            // Releasing shield lets them go voluntarily.
            if (!in.held(BtnShield)) {
                breakGrab(gs, i);
                break;
            }
            // Damage lands once. Cannot test stateFrame == 0: enterState resets
            // it, but the integration step at the end of the SAME frame
            // increments it, so a state never observes 0 inside its own case.
            if (!p.attackConnected) {
                p.attackConnected = true;
                gs.players[p.grabPartner].damage += G.pummelDamage;
            }
            if (p.stateFrame + 1 >= static_cast<uint16_t>(G.pummelFrames)) {
                enterState(p, ActionState::GrabHold);
            }
            break;
        }

            // Distance check -- their x34C/x350. If the victim has drifted too
            // far the hold cannot be maintained.
            const Player &v = gs.players[p.grabPartner];
            if (fx_abs(v.x - p.x) > G.maxHoldDistX || fx_abs(v.y - p.y) > G.maxHoldDistY) {
                breakGrab(gs, i);
            }
            break;
        }

        case ActionState::Pummel: {
            const config::Grab &G = F.grab;
            applyGroundFriction(p);
            if (p.grabPartner >= kMaxPlayers) {
                enterState(p, ActionState::Idle);
                break;
            }
            // Damage lands once. Cannot test stateFrame == 0: enterState resets
            // it, but the integration step at the end of the SAME frame
            // increments it, so a state never observes 0 inside its own case.
            if (!p.attackConnected) {
                p.attackConnected = true;
                gs.players[p.grabPartner].damage += G.pummelDamage;
            }
            if (p.stateFrame + 1 >= static_cast<uint16_t>(G.pummelFrames)) {
                enterState(p, ActionState::GrabHold);
            }
            break;
        }

        case ActionState::Throwing: {
            applyGroundFriction(p);
            // The victim is released MID-animation, at the windup frame, not at
            // the end -- so the thrower is still committed afterwards.
            // Release at or past the windup frame. A strict == would miss it
            // entirely, since stateFrame advances at the end of each frame.
            if (p.stateFrame >= static_cast<uint16_t>(kThrow.windupFrames) &&
                p.grabPartner < kMaxPlayers) {
                releaseThrow(gs, i);
            }
            if (p.stateFrame + 1 >= static_cast<uint16_t>(p.throwFrames)) {
                p.throwDir = 0;
                p.throwFrames = 0;
                enterState(p, ActionState::Idle);
            }
            break;
        }

        case ActionState::GrabRelease:
            applyGroundFriction(p);
            if (p.groundActionFrames > 0) p.groundActionFrames--;
            if (p.groundActionFrames == 0) enterState(p, ActionState::Idle);
            break;

        case ActionState::Grabbed: {
            // Held. No physics of our own -- position is driven by the grabber
            // in the resolution pass. Mash to escape.
            if (p.grabPartner >= kMaxPlayers) {
                enterState(p, ActionState::Idle);
                break;
            }
            const config::Grab &G = fighterOf(gs.players[p.grabPartner]).grab;
            const fx drain = grabMashDrain(in, prev, G);
            const int32_t d = fx_to_int(drain);
            if (p.grabHoldFrames > static_cast<uint16_t>(d)) {
                p.grabHoldFrames -= static_cast<uint16_t>(d);
            } else {
                p.grabHoldFrames = 0;
            }
            if (p.grabHoldFrames == 0) breakGrab(gs, i);
            break;
        }

        case ActionState::Thrown:
            // Guaranteed once started -- no escape. The victim is released by
            // the thrower's Throwing case, which also unlinks them.
            break;

            // --- Shield family ---------------------------------------------------

        case ActionState::ShieldOn:
        case ActionState::Shield:   {
            const config::Shield &S = F.shield;
            ensureShieldInit(p);

            applyGroundFriction(p);
            p.shieldHealth -= S.drainPerFrame;
            if (p.shieldHoldFrames < 0xFF) p.shieldHoldFrames++;

            // Holding it to zero breaks it -- the cost of turtling.
            if (p.shieldHealth <= 0) {
                breakShield(p);
                break;
            }

            // ShieldOn is the grow-in only; the shield is already active from
            // frame 1, so this is not a vulnerable window.
            if (p.state == ActionState::ShieldOn &&
                p.stateFrame + 1 >= static_cast<uint16_t>(S.startupFrames)) {
                enterState(p, ActionState::Shield);
            }

            // Out-of-shield options, in priority order. Spotdodge before roll
            // matches their IASA ordering, so a down-forward diagonal dodges.
            if (checkEscapeInputs(p, in)) break;
            if (in.pressed(BtnJump, prev)) {
                enterState(p, ActionState::Jumpsquat);
                p.jumpHeld = true;
                break;
            }

            // Releasing shield costs the release lag -- but not before the
            // minimum hold has elapsed, so you cannot tap it for free.
            if (!in.held(BtnShield) &&
                p.shieldHoldFrames >= static_cast<uint8_t>(S.minHoldFrames)) {
                p.groundActionFrames = static_cast<uint8_t>(S.releaseFrames);
                enterState(p, ActionState::ShieldOff);
            }
            break;
        }

        case ActionState::ShieldOff:
            // Release lag. This is what makes shielding a commitment rather
            // than a free defensive option. Regen runs here (see regenShield).
            applyGroundFriction(p);
            if (p.groundActionFrames > 0) p.groundActionFrames--;
            if (p.groundActionFrames == 0) enterState(p, ActionState::Idle);
            break;

        case ActionState::ShieldStun:
            // Fully locked -- no buffering out of shieldstun. That lock is what
            // makes hitting a shield safe for the attacker and creates the
            // shield-pressure game.
            applyGroundFriction(p);
            if (p.shieldStunFrames > 0) p.shieldStunFrames--;
            if (p.shieldStunFrames == 0) {
                // Still holding? Back to shielding. Released? Take the lag.
                if (in.held(BtnShield) && p.shieldHealth > 0) {
                    enterState(p, ActionState::Shield);
                } else {
                    p.groundActionFrames = static_cast<uint8_t>(F.shield.releaseFrames);
                    enterState(p, ActionState::ShieldOff);
                }
            }
            break;

        case ActionState::ShieldBroken:
            // Launched helpless out of a broken shield.
            applyGravity(p);
            p.selfVelX = fx_decay(p.selfVelX, F.air.friction);
            break;

        case ActionState::Dizzy: {
            // Mashable, and SHORTER at high damage -- so a shield break early
            // in a stock is a far bigger punish window than one late.
            applyGroundFriction(p);
            if (p.dizzyFrames > 0) p.dizzyFrames--;

            // Two independent drains per frame, as in their GrabMash: any fresh
            // button press, AND any change in stick direction. Both can fire on
            // the same frame, which is why circling the stick works.
            fx drain = kShieldBreak.drainPerFrame;
            if (in.pressed(BtnJump, prev) || in.pressed(BtnAttack, prev) ||
                in.pressed(BtnShield, prev)) {
                drain += kShieldBreak.mashPerInput;
            }
            const bool stickMoved =
                (in.stickX > kStick.deadzone) != (prev.stickX > kStick.deadzone) ||
                (in.stickX < -kStick.deadzone) != (prev.stickX < -kStick.deadzone) ||
                (in.stickY > kStick.deadzone) != (prev.stickY > kStick.deadzone) ||
                (in.stickY < -kStick.deadzone) != (prev.stickY < -kStick.deadzone);
            if (stickMoved) drain += kShieldBreak.mashPerInput;

            const int32_t d = fx_to_int(drain);
            if (p.dizzyFrames > static_cast<uint16_t>(d)) {
                p.dizzyFrames -= static_cast<uint16_t>(d);
            } else {
                p.dizzyFrames = 0;
            }

            if (p.dizzyFrames == 0) {
                p.shieldHealth = F.shield.maxHealth; // shield restored
                enterState(p, ActionState::Idle);
            }
            break;
        }

            // --- Ground escapes --------------------------------------------------

        case ActionState::RollForward:
        case ActionState::RollBack:    {
            const config::GroundEscape &E = F.escape;

            // Fixed authored distance, interpolated over the duration. Melee
            // drives this from the animation root bone, which discards entry
            // momentum and ends dead stopped -- so a roll always covers the same
            // ground no matter how fast you entered it.
            const uint16_t total = static_cast<uint16_t>(E.rollFrames);
            const fx t = fx_div(fxi(p.stateFrame + 1), fxi(total));
            const fx travelled = fx_mul(E.rollDistance, fx_clamp(t, 0, kFxOne));
            p.x = p.rollStartX + travelled * p.escapeDir;
            p.selfVelX = 0;

            // Invulnerable for a window in the middle, not the whole roll.
            if (p.stateFrame == static_cast<uint16_t>(E.rollInvulnStart)) {
                p.invulnFrames = static_cast<uint16_t>(E.rollInvulnFrames);
            }

            if (p.stateFrame + 1 >= total) {
                p.selfVelX = 0; // rolls end stopped, no carryover
                p.escapeDir = 0;
                enterState(p, ActionState::Idle);
            }
            break;
        }

        case ActionState::SpotDodge: {
            const config::GroundEscape &E = F.escape;
            // Deliberately NOT like a roll: plain friction decay, and velocity
            // is NOT zeroed on exit. Spotdodging out of a run slides.
            applyGroundFriction(p);
            if (p.stateFrame == static_cast<uint16_t>(E.dodgeInvulnStart)) {
                p.invulnFrames = static_cast<uint16_t>(E.dodgeInvulnFrames);
            }
            if (p.stateFrame + 1 >= static_cast<uint16_t>(E.dodgeFrames)) {
                enterState(p, ActionState::Idle);
            }
            break;
        }

        case ActionState::Turn:
            // Standing/dash reversal. Facing is already flipped; these frames
            // are the COST of that flip. Jump and attack are available, so a
            // turnaround can be cancelled into an attack the other way.
            if (checkGroundActions(p, in, prev)) break;
            applyGroundFriction(p);
            if (p.groundActionFrames > 0) p.groundActionFrames--;
            if (p.groundActionFrames == 0) {
                // Still holding the new direction? Come out of the turn dashing.
                const bool holding = (in.stickX >= kSmash.deflection && p.facing > 0) ||
                                     (in.stickX <= -kSmash.deflection && p.facing < 0);
                if (holding) {
                    enterDash(p, p.facing);
                } else {
                    enterState(p, ActionState::Idle);
                }
            }
            break;

        default: break;
        }

        // --- Integrate -------------------------------------------------------
        // Motion is the SUM of self-velocity and knockback velocity.
        const fx prevY = p.y;
        const fx vx = p.totalVelX();
        const fx vy = p.totalVelY();
        p.x += vx;
        p.y += vy;
        p.stateFrame++;

        const bool wasGrounded =
            (p.state == ActionState::Idle || p.state == ActionState::Walk ||
             p.state == ActionState::Dash || p.state == ActionState::Run ||
             p.state == ActionState::RunBrake || p.state == ActionState::Turn ||
             p.state == ActionState::ShieldOn || p.state == ActionState::Shield ||
             p.state == ActionState::ShieldOff || p.state == ActionState::ShieldStun ||
             p.state == ActionState::Dizzy || p.state == ActionState::RollForward ||
             p.state == ActionState::RollBack || p.state == ActionState::SpotDodge ||
             p.state == ActionState::Grabbing || p.state == ActionState::GrabHold ||
             p.state == ActionState::Pummel || p.state == ActionState::Throwing ||
             p.state == ActionState::GrabRelease || p.state == ActionState::Landing ||
             p.state == ActionState::Jumpsquat ||
             // Knockdown states are on the ground, so ground
             // friction and ground knockback decay apply.
             p.state == ActionState::DownWait || p.state == ActionState::GetUp ||
             p.state == ActionState::GetUpRoll || p.state == ActionState::GetUpAttack ||
             p.state == ActionState::Tech);
        decayKnockback(p, wasGrounded);

        // --- Walk off the edge -----------------------------------------------
        // A grounded player whose position leaves the platform must start FALLING.
        //
        // This was missing, and it produced two symptoms from one cause: you could
        // walk out past the edge and keep strolling on empty air (nothing ever
        // re-checked whether the floor was still under you), and then jumping from
        // out there dropped you straight through the stage -- because on the way
        // down the landing test correctly found no platform beneath you.
        //
        // Excludes the ledge and knockdown families: those position the body
        // deliberately, sometimes outside the platform bounds (hanging is
        // off-stage by definition), so they must not be reinterpreted as walking
        // off. Jumpsquat is excluded too -- it is a committed takeoff, and being
        // dropped mid-crouch would eat the jump.
        const bool footingMatters =
            (p.state == ActionState::Idle || p.state == ActionState::Walk ||
             p.state == ActionState::Dash || p.state == ActionState::Run ||
             p.state == ActionState::RunBrake || p.state == ActionState::Turn ||
             p.state == ActionState::ShieldOn || p.state == ActionState::Shield ||
             p.state == ActionState::ShieldOff || p.state == ActionState::ShieldStun ||
             p.state == ActionState::Dizzy || p.state == ActionState::SpotDodge ||
             p.state == ActionState::Grabbing || p.state == ActionState::GrabHold ||
             p.state == ActionState::Pummel || p.state == ActionState::Throwing ||
             p.state == ActionState::GrabRelease || p.state == ActionState::Landing ||
             p.state == ActionState::AttackGround || p.state == ActionState::GetUp ||
             p.state == ActionState::GetUpRoll || p.state == ActionState::GetUpAttack ||
             p.state == ActionState::Tech || p.state == ActionState::DownWait);
        if (footingMatters) {
            const Stage &st = gs.stage;
            const bool stillSupported = (p.x >= st.platformLeft && p.x <= st.platformRight);
            if (!stillSupported) {
                p.selfVelY = 0; // start the fall from rest, not from stale vy
                p.fastFalling = false;
                p.attackId = ATK_NONE;
                p.charging = false;
                p.chargeFrames = 0;
                enterState(p, ActionState::Airborne);
            }
        }

        // --- Ledge grab ------------------------------------------------------
        // Checked BEFORE ground collision, because a player descending past the
        // lip is inside both windows on the same frame. Ledge must win, or
        // recovering from below would clip onto the stage instead of catching.
        //
        // Restricted to states where the player is in ACTIVE control.
        //
        // Hitstun and Bounce are deliberately EXCLUDED. Allowing a grab from
        // hitstun means being launched off-stage auto-snaps you to the ledge with
        // no input at all -- a free save that would gut the KO system, since almost
        // nothing near the ledge could kill. You must recover out of hitstun first
        // and then reach the ledge under your own control.
        //
        // Aerials and air dodges are excluded for the same reason at a smaller
        // scale: recovery should be deliberate, not magnetic.
        const bool canGrabLedge =
            (p.state == ActionState::Airborne || p.state == ActionState::LedgeJump ||
             // Helpless players CAN still catch a ledge --
             // otherwise an air dodge near the stage would be
             // an unrecoverable death sentence.
             p.state == ActionState::FallHelpless);
        if (canGrabLedge) {
            const int8_t side = detectLedgeGrab(gs, p, F.ledge);
            if (side >= 0) {
                attachToLedge(gs, p, side, F.ledge);
                checkBlastZones(gs, i);
                continue; // skip ground collision this frame -- we are hanging
            }
        }

        // --- Ground collision (swept) ----------------------------------------
        // Tested against the movement segment [prevY, p.y] rather than the final
        // point, so fast-falling can't tunnel through the platform. Landing only
        // happens while descending, so rising through the stage from below works.
        const Stage &s = gs.stage;
        const bool overPlatform = (p.x >= s.platformLeft && p.x <= s.platformRight);
        const bool crossedFloor = (prevY <= s.groundY + s.landSnapEpsilon && p.y >= s.groundY);
        if (overPlatform && vy >= 0 && crossedFloor) {
            // Impact speed must be sampled BEFORE any velocity is zeroed -- the
            // knockdown decision is a function of the knockback SURVIVING at the
            // moment of contact, so clearing it first would destroy the input.
            const fx impactSpeed = fx_sqrt(fx_mul(p.kbVelX, p.kbVelX) + fx_mul(p.kbVelY, p.kbVelY));
            const bool wasInHitstun =
                (p.state == ActionState::Hitstun || p.state == ActionState::Bounce);

            p.y = s.groundY;
            p.selfVelY = 0;
            p.fastFalling = false;
            p.airJumps = 0;

            // Only vertical velocity is zeroed. Horizontal momentum survives --
            // that is precisely what makes wavedashing work.
            //
            // Landing lag is decided HERE, once, and latched. See the Landing case.
            const uint8_t fullLag = static_cast<uint8_t>(F.landing.aerialLagFrames);

            if (wasInHitstun) {
                // The knockdown decision, mirroring ftCo_Damage_Coll: branch on the
                // knockback speed remaining at impact, NOT on the hit that caused
                // it. The same attack therefore produces different outcomes
                // depending on how far you flew and how much has decayed -- slam in
                // immediately and you go down, drift it off and you just land.
                const Knockdown &K = F.knockdown;

                // A tech beats every other outcome. Window open and not locked out.
                if (p.techWindow > 0 && p.techLockout == 0) {
                    p.techWindow = 0;
                    p.kbVelX = 0;
                    p.kbVelY = 0;
                    p.hitstunFrames = 0;
                    p.bounceCount = 0;
                    p.invulnFrames = static_cast<uint16_t>(K.techInvulnFrames);
                    enterState(p, ActionState::Tech);
                } else if (impactSpeed >= K.bounceThreshold && p.bounceCount == 0) {
                    // Hard slam: bounce back into the air, then come down into a
                    // knockdown. bounceCount caps it at one so a fast character
                    // can't be pinballed indefinitely.
                    p.bounceCount++;
                    p.kbVelY = fx_mul(p.kbVelY, K.bounceVelYMult);
                    p.kbVelX = fx_mul(p.kbVelX, K.bounceVelXMult);
                    p.hitstunFrames = 0;
                    // Lift clear of the floor so the swept test doesn't re-trigger
                    // on the very next frame.
                    p.y = s.groundY - fxi(1);
                    enterState(p, ActionState::Bounce);
                } else if (impactSpeed >= K.hardLandThreshold || p.bounceCount > 0) {
                    // Moderate impact, or the landing after a bounce: knocked down.
                    p.kbVelX = 0;
                    p.kbVelY = 0;
                    p.hitstunFrames = 0;
                    p.bufferedButtons = 0;
                    enterState(p, ActionState::DownWait);
                } else {
                    // Soft landing: the knockback has decayed enough that this is
                    // just a landing. Free recovery.
                    p.kbVelY = 0;
                    p.hitstunFrames = 0;
                    p.bounceCount = 0;
                    p.landingLag = fullLag;
                    enterState(p, ActionState::Landing);
                }
            } else if (p.state == ActionState::AttackAir) {
                // Only aerials are L-cancellable -- that's the whole technique.
                //
                // Landing lag comes from the AERIAL, not one shared number. Melee
                // has five separate values (landingairn_lag, landingairf_lag, ...)
                // so "which aerial is safe to land with" is a real decision: a fast
                // neutral-air recovers quickly, a committal spike does not. Zero in
                // the table means "use the character's default".
                p.kbVelY = 0;
                const int moveLag = attackOf(p, p.attackId).landingLag;
                const uint8_t baseLag = (moveLag > 0) ? static_cast<uint8_t>(moveLag) : fullLag;
                p.landingLag = (p.lcancelTimer > 0)
                                   ? static_cast<uint8_t>(baseLag / F.landing.lcancelDivisor)
                                   : baseLag;
                p.attackId = ATK_NONE;
                p.chargeFrames = 0;
                p.charging = false;
                enterState(p, ActionState::Landing);
            } else if (p.state == ActionState::ShieldBroken) {
                p.kbVelY = 0;
                enterState(p, ActionState::Dizzy);
            } else if (p.state == ActionState::FallHelpless) {
                // Landing out of helplessness costs extra -- the commitment has to
                // be paid for on the way out too, not just on the way in.
                p.kbVelY = 0;
                p.landingLag = static_cast<uint8_t>(F.airDodge.helplessLandingLag);
                enterState(p, ActionState::Landing);
            } else if (p.state == ActionState::Airborne || p.state == ActionState::AirDodge) {
                p.kbVelY = 0;
                p.landingLag = fullLag;
                enterState(p, ActionState::Landing);
            } else {
                p.kbVelY = 0;
            }
        }

        checkBlastZones(gs, i);
    }

    // Grab pairs resolve last, once every player has advanced -- see the function
    // comment for why this cannot live in the loop above.
    resolveGrabPairs(gs);
}

uint32_t checksum(const GameState &gs) {
    // FNV-1a over simulation-relevant fields. Field-by-field on purpose: hashing
    // the raw struct would include uninitialized padding bytes and report
    // spurious desyncs for identical states.
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) {
        for (int b = 0; b < 4; ++b) {
            h ^= (v >> (b * 8)) & 0xFFu;
            h *= 16777619u;
        }
    };
    mix(gs.tick);
    for (const Player &p : gs.players) {
        mix(p.active ? 1u : 0u);
        mix(static_cast<uint32_t>(p.x));
        mix(static_cast<uint32_t>(p.y));
        mix(static_cast<uint32_t>(p.selfVelX));
        mix(static_cast<uint32_t>(p.selfVelY));
        mix(static_cast<uint32_t>(p.kbVelX));
        mix(static_cast<uint32_t>(p.kbVelY));
        mix(static_cast<uint32_t>(p.damage));
        mix(static_cast<uint32_t>(p.state));
        mix(p.stateFrame);
        mix(static_cast<uint32_t>(p.stocks));
        mix(static_cast<uint32_t>(p.facing));
        mix(p.hitstunFrames);
        mix(p.attackId);
        mix(p.attackFrame);
        mix(p.chargeFrames);
        mix(p.techWindow);
        mix(p.techLockout);
        mix(p.bounceCount);
        mix(static_cast<uint32_t>(p.rollDir));
        mix(p.bufferedButtons);
        mix(p.hitlagFrames);
        mix(p.groundActionFrames);
        mix(p.sdiNudges);
        mix(static_cast<uint32_t>(p.shieldHealth));
        mix(p.shieldStunFrames);
        mix(p.dizzyFrames);
        mix(static_cast<uint32_t>(p.escapeDir));
        mix(static_cast<uint32_t>(p.pendingPushX));
        mix(p.grabPartner);
        mix(p.isGrabber ? 1u : 0u);
        mix(p.grabHoldFrames);
        mix(p.throwDir);
        mix(p.throwFrames);
        mix(static_cast<uint32_t>(p.ledgeSide));
        mix(p.ledgeHangFrames);
        mix(p.ledgeCooldown);
        mix(p.ledgeActionFrames);
        mix(p.charId);
    }
    return h;
}

} // namespace tf
