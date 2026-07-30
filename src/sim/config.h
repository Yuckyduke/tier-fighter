#pragma once
#include "fixed.h"
#include <cstdint>

// ALL game-design tunables live here, grouped by what they control.
//
// Two rules for this file:
//   1. Nothing here is a float. Every value is an exact rational via fx_ratio,
//      so the simulation stays bit-identical across machines (see fixed.h).
//   2. The simulation contains no numeric literals -- if you find one in
//      sim.cpp, it belongs here instead.
//
// Tuning these numbers IS designing the game. The formulas in sim.cpp are
// structural; the feel comes from this file.

namespace tf::config {

// --- Jumping -----------------------------------------------------------------
// Jumpsquat is the crouch before takeoff. Its length is the short-hop window:
// release jump before it ends and you get a hop instead of a full jump. Short
// makes the game twitchy and precise; long makes hops easy but jumps sluggish.
struct Jump {
    int jumpsquatFrames = 4;
    fx fullVelocity = fx_ratio(-63, 10); // negative is up (+Y is down)
    fx hopVelocity = fx_ratio(-38, 10);  // jump released during jumpsquat
    fx airJumpVelocity = fx_ratio(-58, 10);
    int maxAirJumps = 1;
};

// --- Ground movement ---------------------------------------------------------
// Friction is what makes wavedashing worth doing: low friction means momentum
// carried out of an air dodge slides a long way.
struct GroundMove {
    // --- Walking -------------------------------------------------------------
    // Walk RAMPS rather than snapping: an initial velocity on the first frame,
    // then acceleration toward the cap. Instant-on movement is the single biggest
    // giveaway that a fighter's ground game is shallow.
    fx walkInitVel = fx_ratio(4, 10);
    fx walkAccel = fx_ratio(16, 100);
    fx walkSpeed = fx_ratio(14, 10);

    // --- Dash and Run --------------------------------------------------------
    // These are DISTINCT states, and the distinction is the mechanic:
    //   Dash is INTERRUPTIBLE  -> flick the other way and you re-enter it
    //   Run is NOT             -> leaving it requires RunBrake
    //
    // That is the whole basis of dash-dancing. ftCo_Dash_CheckInput re-enters Dash
    // on every fresh stick flick (using the same recency timer that separates a
    // smash from a tilt); Run has no such entry. Collapse the two into one
    // snap-to-speed state and there is nothing to dance with.
    fx dashInitVel = fx_ratio(22, 10); // impulse on entering a dash
    fx dashAccel = fx_ratio(22, 100);
    fx dashSpeed = fx_ratio(34, 10);

    // Frames a dash lasts before it commits into a run. This window IS the
    // dash-dance timing: flick within it and you get a fresh dash instead.
    int dashFrames = 12;

    // Run acceleration TAPERS as you approach top speed, scaled by
    // (1 - vel/target) -- hard at low speed, easing into the cap. From
    // ftCo_Run_Phys. Without the taper a run reaches top speed abruptly and feels
    // like a switch rather than a build-up.
    fx runAccel = fx_ratio(18, 100);
    fx runTaper = fx_ratio(12, 10);

    // Braking out of a run, and turning on the spot. Both are COMMITMENTS -- a
    // free instant reversal removes the risk from committing to a direction, and
    // with it most of the ground game's texture.
    int runBrakeFrames = 12;
    fx runBrakeFriction = fx_ratio(28, 100);
    int turnFrames = 6;     // frames_to_change_direction_on_standing_turn
    int dashTurnFrames = 3; // reversing out of a dash is faster than standing

    // Hard ceiling on ground speed, separate from dashSpeed -- momentum from a
    // wavedash or a launch can exceed what you could accelerate to.
    fx maxHorizontal = fx_ratio(42, 10);

    // Friction is SPEED-DEPENDENT, in two tiers. From ft_80084F3C:
    //
    //     friction = gr_friction
    //     if |groundVel| > walkSpeed:  friction *= fastMultiplier
    //
    // Above walking pace you brake hard; at or below it, the low base value lets you
    // keep gliding. That asymmetry is the whole answer to two complaints that look
    // contradictory:
    //
    //   - A single LOW value (0.08) meant a dash coasted 43 frames -- ice.
    //   - A single HIGH value (0.24) stopped the dash in 15 frames, but killed the
    //     wavedash GLIDE. Total distance actually went up, yet it felt worse,
    //     because the slide decayed uniformly and died abruptly instead of
    //     carrying. Distance was the wrong thing to measure; the long low-speed
    //     tail is what reads as a wavedash.
    //
    // Two tiers give both: fast motion brakes quickly, slow motion glides.
    fx friction = fx_ratio(9, 100);
    fx fastMultiplier = fxi(3); // applied above walkSpeed
};

// --- Air movement ------------------------------------------------------------
// Acceleration rather than snapping to top speed is what makes drifting feel
// weighty and keeps committed jumps committed.
struct AirMove {
    // Drift TARGET -- the fastest you can accelerate yourself to (air_drift_max).
    fx maxSpeed = fx_ratio(19, 10);

    // Drift acceleration has two terms, as in ftCommon_8007D28C:
    //     accel = stick * stickMul + sign(stick) * base
    // A flat floor plus a stick-proportional term, so a light tilt drifts gently
    // and a full deflection drifts hard. A single constant makes drift
    // all-or-nothing past the deadzone, which loses the analog nuance entirely.
    fx acceleration = fx_ratio(12, 100); // the flat term (aerial_drift_base)
    fx stickMul = fx_ratio(10, 100);     // the proportional term

    fx friction = fx_ratio(2, 100);
    fx gravity = fx_ratio(23, 100);
    fx termVelocity = fx_ratio(60, 10);
    fx fastFallSpeed = fx_ratio(95, 10); // instant on hard stick down, not gradual

    // Hard CEILING, distinct from the drift target (air_max_horizontal_velocity).
    // Momentum from a launch or a wavedash can exceed what you could accelerate to,
    // but never this. With one value those two ideas collapse and carried momentum
    // gets clamped down to walking pace.
    fx maxHorizontal = fx_ratio(34, 10);
};

// --- Air dodge ---------------------------------------------------------------
// The wavedash enabler. Directional dodges launch along the stick; horizontal
// momentum survives the landing (nothing zeroes vx), so a shallow downward dodge
// slides you across the ground. Raise `speed` or lower GroundMove::friction and
// wavedashes go further.
struct AirDodge {
    // Raised alongside GroundMove::friction -- see the note there. This is the
    // wavedash's initial velocity, so it is what keeps the technique worthwhile
    // once the ground actually grips.
    fx speed = fx_ratio(65, 10);
    fx momentumDecay = fx_ratio(14, 100);
    int durationFrames = 28;
    int invulnFrames = 20;

    // ONE air dodge per airborne period, and it costs your remaining jumps.
    //
    // Melee enforces this structurally rather than with a counter: EscapeAir exits
    // into FallSpecial (via ftCo_80096900), a distinct helpless fall state whose
    // input handler does NOT include the air-dodge check -- so there is simply no
    // way to dodge again until you touch the ground. FallSpecial also calls
    // ftCommon_UseAllJumps, consuming your jumps on the way in.
    //
    // That structure is what makes an air dodge a COMMITMENT. Without it, repeated
    // upward dodges give unlimited vertical recovery and nobody can ever be edge-
    // guarded -- which is exactly what our unlimited version allowed.
    bool oneUsePerAirtime = true;
    bool consumesJumps = true;

    // Landing lag on touching down while helpless. Higher than a normal landing:
    // the commitment has to cost something on the way out as well.
    int helplessLandingLag = 16;
};

// --- Landing / L-cancel ------------------------------------------------------
// Pressing shield within the window before landing halves aerial landing lag.
// One conditional, and it's what lets offense flow instead of stalling.
struct Landing {
    int aerialLagFrames = 10;
    int lcancelWindow = 7;
    int lcancelDivisor = 2;
};

// --- Hitlag ------------------------------------------------------------------
// The freeze on contact. BOTH fighters stop for the same number of frames.
//
// Mirrors ftCommon_CalcHitlag:
//     frames = damage * perDamage + base
//     crouching multiplies it further (crouchMult)
//     clamped to [minFrames, maxFrames]
//
// This is most of what makes a heavy hit FEEL heavy -- the momentary stop reads as
// force in a way that knockback alone does not. Without it, hits resolve and
// launch on the same frame and everything feels weightless.
//
// It is also the window in which SDI happens: the decomp sets allow_sdi while
// hitlag runs, so smash-directional-influence is only possible because the freeze
// exists. Hitlag first, SDI on top of it.
struct Hitlag {
    fx perDamage = fx_ratio(4, 10);   // frames per 1% damage
    fx base = fxi(4);                 // flat floor so even a jab registers
    fx crouchMult = fx_ratio(12, 10); // crouching lengthens it (see kb_squat_mul)
    int minFrames = 2;
    int maxFrames = 30; // their x194_unkHitLagFrames equivalent

    // The attacker's freeze as a fraction of the defender's. Equal in Melee; kept
    // separate so it can be tuned -- a shorter attacker freeze rewards aggression.
    fx attackerFraction = kFxOne;
};

// --- Shield ------------------------------------------------------------------
// Mirrors the Guard* family. Four states: startup, hold, release, and shieldstun.
//
// Shield HEALTH is one value with three independent flows, and all three matter:
//   - drains every frame it is held
//   - REGENERATES every frame it is not active (including during release lag)
//   - takes damage on hit: a proportional term PLUS a flat cost per hit
// The flat cost is what makes many weak hits meaningfully worse than one big one.
//
// A subtlety worth preserving: shieldstun scales with the LARGEST single hit
// absorbed, while health loss scales with the SUM. Two different accumulators --
// collapsing them into one would make multi-hit moves behave wrongly.
//
// Deliberately NOT implemented: powershield (its own timer web plus reflect
// hitboxes -- a feature in its own right), analog light-shield gradations (we have
// digital buttons; hardness is left as a hook), and shield tilting (positional
// only, no hurtbox effect).
struct Shield {
    fx maxHealth = fxi(60);
    fx drainPerFrame = fx_ratio(15, 100); // cost of simply holding it
    fx regenPerFrame = fx_ratio(7, 100);  // while NOT shielding
    // Damage taken: proportional to the hit, plus a flat cost per hit.
    fx damageScale = fx_ratio(12, 10);
    fx damageFlat = fx_ratio(30, 10);

    int startupFrames = 3;  // shield is already active; this is the grow-in
    int releaseFrames = 12; // the lag that makes shielding a commitment
    int minHoldFrames = 4;  // cannot release before this, even if you let go

    // Shieldstun: frames = largestHit * scale + flat.
    fx stunScale = fx_ratio(45, 100);
    fx stunFlat = fx_ratio(20, 10);
    int stunMaxFrames = 30;

    // Pushback on the shielding player, away from the attacker. Capped so a big
    // hit cannot shove someone across the stage.
    fx pushbackScale = fx_ratio(18, 100);
    fx pushbackCap = fx_ratio(28, 10);
    // Pushback on the ATTACKER, scaled by the damage they dealt. This is what
    // makes hitting a shield a positional loss rather than free pressure.
    fx attackerPushScale = fx_ratio(9, 100);
    fx attackerPushFlat = fx_ratio(4, 10);
};

// --- Shield break / dizzy ----------------------------------------------------
// Running the shield to zero launches you upward helpless, then leaves you dizzy.
//
// The dizzy duration SHRINKS as damage rises (their x2F8 - percent formula) and is
// mashable. Both are deliberate: a shield break at low percent is a much longer
// punish window than one at high percent, which keeps the mechanic from being an
// automatic kill late in a stock.
struct ShieldBreak {
    fx launchVelY = fx_ratio(-45, 10);
    int dizzyBase = 240;                   // at 0% damage
    fx dizzyPerDamage = fx_ratio(-12, 10); // negative: more damage, shorter dizzy
    int dizzyMin = 60;
    // Mash drain, matching their two independent inputs per frame.
    fx mashPerInput = fxi(6);
    fx drainPerFrame = fxi(1);
};

// --- Ground escapes (roll / spotdodge) ---------------------------------------
// Mirrors the Escape* family. Two mechanics with a deliberate asymmetry:
//
//   ROLL travels a FIXED authored distance. Melee drives it from the animation's
//   root-bone delta, which discards entry momentum and ends dead stopped. We
//   interpolate over the duration for the same result: a roll always covers the
//   same ground regardless of how fast you entered it.
//
//   SPOTDODGE is plain friction decay and KEEPS residual momentum -- it does not
//   zero velocity on exit either. So spotdodging out of a run slides, while
//   rolling never does. That asymmetry is intentional in the original.
//
// Invulnerability in Melee is a hurtbox flag set by animation timeline events, so
// per-character windows cost nothing there. We use a single invuln countdown at
// lower resolution, which is the same idea -- and since the frame numbers are ours
// to choose anyway, it is a simplification rather than a compromise.
struct GroundEscape {
    int rollFrames = 32;
    int rollInvulnStart = 4; // vulnerable on the first few frames
    int rollInvulnFrames = 16;
    fx rollDistance = fxi(80);

    int dodgeFrames = 24;
    int dodgeInvulnStart = 2;
    int dodgeInvulnFrames = 14;
};

// --- SDI (smash directional influence) --------------------------------------
// Flick the stick during hitlag and you shift POSITION -- not velocity.
//
// Mirrors ftCo_Damage_OnEveryHitlag, and every detail of that function matters:
//
//   1. It runs on EVERY hitlag frame, not once per hit. A long freeze offers
//      multiple nudges if you can re-flick fast enough.
//   2. The magnitude gate is on the stick VECTOR (squared length), not per-axis,
//      so a diagonal qualifies even when neither axis alone clears the threshold.
//   3. EITHER axis flicking fresh is enough (an ||, not an &&) -- a clean
//      single-axis flick works.
//   4. It adds directly to position. That is why SDI reads as an instant
//      displacement rather than as drift: it sidesteps velocity entirely.
//   5. Both stick timers saturate on success, so one flick buys exactly one nudge.
//      Multi-SDI requires genuinely re-flicking mid-freeze, which is what makes it
//      a hard technique rather than a held-direction freebie.
//
// Why it exists: hitlag is dead time for the defender otherwise. SDI turns the
// freeze into a decision -- escape a multi-hit move, adjust where you get launched
// from, or drift toward the stage. It only works because hitlag exists.
struct SDI {
    // Minimum stick magnitude, as a fraction of full deflection.
    fx minStickMag = fx_ratio(7, 10);
    // Frames since the stick crossed that still counts as a flick. Shares the same
    // recency timers as smash-vs-tilt and fast-fall.
    int stickWindow = 3;
    // World units shifted per unit of stick deflection.
    fx posScale = fx_ratio(6, 10);
    // Cap per hitlag freeze, so a long freeze cannot be ridden across the stage.
    int maxNudgesPerHitlag = 4;
};

// --- Knockdown / tech --------------------------------------------------------
// What happens when a player in hitstun contacts the ground.
//
// Mirrors ftCo_Damage_Coll in the decomp, which branches on the knockback velocity
// REMAINING AT THE MOMENT OF IMPACT -- not on the hit that caused it:
//
//   |kbVel| >= bounceThreshold    -> bounce, then knocked down
//   |kbVel| >= hardLandThreshold  -> ordinary landing lag
//   otherwise                     -> free recovery
//
// That the decision reads surviving velocity is the elegant part: the same attack
// produces different outcomes depending on how far you flew and how much the
// knockback has decayed. Slam into the ground immediately and you're knocked down;
// drift long enough for it to bleed off and you simply land. It falls out of the
// kbVel decay for free.
//
// TECH (the decomp's Passive state) is the counterplay: press shield just before
// impact and you recover instantly with brief invulnerability instead of being
// knocked down. It's the defensive skill that makes combos escapable -- without it,
// landing in hitstun has no interaction at all.
//
// The lockout is what stops mashing from being optimal. A tech attempt that never
// meets the ground expires and locks you out, so shield has to be timed rather than
// held down.
struct Knockdown {
    fx bounceThreshold = fx_ratio(40, 10);
    fx hardLandThreshold = fx_ratio(15, 10);

    int techWindow = 20;        // frames after a shield press a tech can land
    int techLockoutFrames = 40; // penalty for a tech attempt that never connects
    int techFrames = 26;        // length of the tech recovery animation
    int techInvulnFrames = 20;

    // The bounce off the ground. Vertical velocity reverses (scaled), horizontal
    // is preserved (scaled), so a hard diagonal slam skips along the stage.
    fx bounceVelYMult = fx_ratio(-45, 100);
    fx bounceVelXMult = fx_ratio(50, 100);

    int downWaitMinFrames = 12; // earliest you may act while lying down
    int downWaitMaxFrames = 90; // forced get-up, so nobody can lie there forever

    int getUpFrames = 22; // neutral stand-up
    int getUpInvulnFrames = 12;
    int rollFrames = 30; // roll left/right
    int rollInvulnFrames = 20;
    fx rollSpeed = fx_ratio(38, 10);
    int getUpAttackFrames = 34; // stand-up attack; hitbox from ATK_GETUP
};

// --- Ledges ------------------------------------------------------------------
// Grabbing and getting off the ledge. Mirrors the decomp's Cliff* family.
//
// Three findings from ftCo_CliffWait / ftCo_CliffClimb / ftCo_CliffEscape shape
// this, and each one exists to stop a degenerate strategy:
//
//   1. LEDGE COOLDOWN. Leaving the ledge -- by climbing, by timing out, or by
//      getting hit -- sets a cooldown during which you cannot regrab. Without it,
//      hopping off and immediately regrabbing refreshes invulnerability forever
//      and the ledge becomes an invincible camping spot.
//
//   2. HANG TIMEOUT. Hanging has a countdown; at zero you are forced off and take
//      the cooldown. So the ledge is a temporary refuge, not a hiding place.
//
//   3. SLOW vs QUICK BY DAMAGE. Every get-up option has a fast version and a slow
//      one, chosen by comparing your damage to a threshold -- below it you get the
//      quick version, at or above it the slow, more punishable one. This is a
//      lovely piece of design: being badly damaged makes recovery itself riskier,
//      so the pressure compounds exactly when you can least afford it.
//
// Their 12 Cliff* states collapse to 6 here because slow/quick is a PARAMETER
// (which frame count to use), not a separate state machine.
struct Ledge {
    // Grab detection. A ledge is grabbable if the player is airborne, descending
    // or level, within this box of the ledge point, and on the outward side of it.
    fx grabReachX = fxi(26);    // horizontal distance from the ledge point
    fx grabReachUp = fxi(20);   // how far ABOVE the ledge you can still catch
    fx grabReachDown = fxi(58); // how far BELOW -- the bulk of the grab window

    // Must you be FACING the ledge to catch it?
    //
    // Melee requires this, and it matters: it turns recovery into a deliberate act
    // rather than a proximity check. Drifting backwards past the edge does NOT save
    // you -- you have to turn around, which costs time you may not have. It also
    // makes reverse-facing recoveries a real skill expression.
    //
    // (The decomp corrects facing on catch in ftCliffCommon_80081370, snapping you
    // to face the stage. That is presentation. The gate itself lives in the
    // Collide_LeftLedgeGrab / Collide_RightLedgeGrab env flags, which are set per
    // side, so which ledge you can grab depends on your approach.)
    bool requireFacing = true;

    // Where the body sits while hanging, relative to the ledge point.
    fx hangOffsetX = fxi(14); // outward from the edge
    fx hangOffsetY = fxi(36); // below the lip

    int hangFrames = 480;      // 8s at 60fps, then forced off (finding 2)
    int grabInvulnFrames = 30; // brief invulnerability on catching
    int cooldownFrames = 32;   // regrab lockout (finding 1)

    // Damage at or above which the SLOW variants are used (finding 3).
    fx slowThreshold = fxi(100);

    // Climb up onto the stage.
    int climbQuickFrames = 24;
    int climbSlowFrames = 46;

    // Roll onto the stage past the edge -- invulnerable for most of it.
    int rollQuickFrames = 32;
    int rollSlowFrames = 56;
    int rollInvulnFrames = 22;
    fx rollDistance = fxi(54);

    // Get-up attack from the ledge; uses ATK_LEDGE.
    int attackQuickFrames = 38;
    int attackSlowFrames = 58;

    // Jumping off. Releases you into normal airborne control.
    int jumpFrames = 12;
    fx jumpVelY = fx_ratio(-58, 10);
    fx jumpVelX = fx_ratio(12, 10); // slight outward drift
};

// --- Knockback ---------------------------------------------------------------
// Coefficients of the standard Smash knockback relationship. See computeKnockback
// in sim.cpp for the formula these plug into.
struct Knockback {
    fx damageDivisor = fxi(10);        // p / 10
    fx damageProductDivisor = fxi(20); // p*d / 20
    fx weightNumerator = fxi(200);     // 200 / (w + weightOffset)
    fx weightOffset = fxi(100);
    fx scale = fx_ratio(14, 10);
    fx floor = fxi(18); // weak hits still move you
    fx growthDivisor = fxi(100);
    fx velocityScale = fx_ratio(9, 100); // knockback units -> px/frame

    // Knockback velocity is tracked separately from self-velocity (see Player in
    // state.h) and decays along its own vector. Below `snapToZero` it is cleared
    // outright, so launched players return to full control cleanly instead of
    // drifting on a vanishing remainder forever.
    fx airDecay = fx_ratio(51, 1000);
    fx groundDecay = fx_ratio(20, 100);
    fx snapToZero = fx_ratio(1, 10);
};

// --- Hitstun ----------------------------------------------------------------
// Hitstun proportional to knockback is where combos come from: a big hit buys
// the attacker enough frames to follow up.
struct Hitstun {
    fx perKnockback = fx_ratio(4, 10);
    int minFrames = 4;
    int maxFrames = 90;
};

// --- Directional influence ---------------------------------------------------
// Holding perpendicular to your launch bends the trajectory, making survival an
// active skill rather than a dice roll.
struct DirectionalInfluence {
    int maxBendDegrees = 18;
};

// --- Body / collision --------------------------------------------------------
// Melee models the fighter body as an ECB: four points (top/bottom/left/right)
// forming a diamond, swept from the previous position to the desired one so fast
// movement can't tunnel through a platform. We use a swept AABB here -- same
// anti-tunneling property, simpler math -- and the diamond shape is a later
// refinement that mainly affects ledge-grab and wall-interaction feel.
struct Body {
    fx halfWidth = fxi(16);
    fx height = fxi(46);
    fx weight = fxi(100); // 100 is neutral in the knockback weight term
};

// --- Respawn -----------------------------------------------------------------
struct Respawn {
    int waitFrames = 48;
    int invulnFrames = 90;
    fx spawnHeight = fxi(120); // above stage floor
};

// --- Input thresholds --------------------------------------------------------
// Stick is an integer in [-kStickRange, +kStickRange], so it serializes exactly
// with no float anywhere in the transport.
//
// kStickRange is referenced wherever the stick is normalized. It is named rather
// than written as a literal because changing it silently breaks every
// normalization site at once.
constexpr int kStickRange = 100;

struct StickThresholds {
    int8_t deadzone = 28; // past this, an axis counts as deflected
    int8_t hard = 62;     // past this, it's a dash / fast-fall input
};

// --- Attacks -----------------------------------------------------------------
// A hitbox is live only on frames [startup, startup+active). That gap is what
// makes attacks readable and whiff-punishable -- it's the core grammar of a
// fighting game: commitment, threat window, recovery.
//
// baseKnockback vs knockbackGrowth is the key design knob:
//   high base + low growth  -> reliable kill move, consistent at any percent
//   low base + high growth  -> combo starter early, kill move at high percent
//
// The move set follows the standard matrix, which the decomp's naming confirms
// (ftCo_Attack1 = jab, AttackS3/Hi3/Lw3 = tilts, AttackS4/Hi4/Lw4 = smashes,
// AttackAir = aerials). Tilts and smashes are the SAME four-direction matrix at
// two strength tiers, sharing one resolution path.
//
// IMPORTANT: every number below is MINE, not Melee's. The decomp contains the
// attack STATE MACHINES but no frame data -- per-character damage/angle/knockback
// live in the disc's Pl**.dat files, which it doesn't distribute. So these are
// designed to be internally coherent (slow moves hit harder, aerials trade reach
// for speed), not to reproduce any specific character.
struct AttackData {
    int startup, active, total;
    fx reachX, reachY; // hitbox offset from character center
    fx radius;
    fx damage;
    fx baseKnockback;
    fx knockbackGrowth;
    int angleDeg;          // 0 = forward, 90 = straight up, 270 = spike down
    fx selfVelX, selfVelY; // momentum imparted on the attacker
    bool chargeable;       // smashes only

    // Landing lag for THIS aerial. Melee has five separate values
    // (landingairn_lag, landingairf_lag, ...) rather than one shared number, so
    // "which aerial is safe to land with" is a real decision. Zero means "use the
    // character's default" -- ground moves and non-aerials leave it zero.
    int landingLag;
};

// Every attack in the game, indexed by AttackId (see state.h).
enum AttackIndex : uint8_t {
    ATK_NONE = 0,
    ATK_JAB,
    ATK_TILT_SIDE,
    ATK_TILT_UP,
    ATK_TILT_DOWN,
    ATK_SMASH_SIDE,
    ATK_SMASH_UP,
    ATK_SMASH_DOWN,
    ATK_AIR_NEUTRAL,
    ATK_AIR_FORWARD,
    ATK_AIR_BACK,
    ATK_AIR_UP,
    ATK_AIR_DOWN,
    ATK_GETUP, // rising attack from knockdown; hits both sides
    ATK_LEDGE, // get-up attack from hanging on a ledge
    ATK_COUNT,
};

// --- Attack table: "Scout" ---------------------------------------------------
// A light, fast, low-commitment character. Order MUST match AttackIndex.
constexpr AttackData kScoutAttacks[ATK_COUNT] = {
    // ATK_NONE -- never resolved, exists so index 0 is a safe default.
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, 0},

    // Jab: the fastest option. Low commitment, low reward -- your poke.
    {/*su*/ 3, /*act*/ 2, /*tot*/ 16,
     /*rX*/ fxi(32), /*rY*/ fxi(-16), /*rad*/ fxi(22),
     /*dmg*/ fxi(3), /*bKB*/ fxi(10), /*kbG*/ fxi(30), /*ang*/ 40,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},

    // --- Tilts: moderate speed, moderate reward. The workhorse pokes. --------
    {/*su*/ 5, /*act*/ 3, /*tot*/ 22, // TILT_SIDE
     /*rX*/ fxi(40), /*rY*/ fxi(-18), /*rad*/ fxi(24),
     /*dmg*/ fxi(8), /*bKB*/ fxi(14), /*kbG*/ fxi(58), /*ang*/ 32,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},

    {/*su*/ 4, /*act*/ 4, /*tot*/ 24, // TILT_UP
     /*rX*/ fxi(12), /*rY*/ fxi(-52), /*rad*/ fxi(26),
     /*dmg*/ fxi(7), /*bKB*/ fxi(12), /*kbG*/ fxi(64), /*ang*/ 84,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},

    // Low angle: pops the opponent sideways at ground level rather than launching,
    // which is what makes a down tilt a combo starter instead of a finisher.
    {/*su*/ 5, /*act*/ 3, /*tot*/ 20, // TILT_DOWN
     /*rX*/ fxi(38), /*rY*/ fxi(2), /*rad*/ fxi(22),
     /*dmg*/ fxi(6), /*bKB*/ fxi(10), /*kbG*/ fxi(48), /*ang*/ 12,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},

    // --- Smashes: slow, committal, chargeable. Your kill moves. --------------
    // High startup AND high total means whiffing one is a real punish window.
    //
    // Note the LOW base knockback with HIGH growth. That profile matters more than
    // the raw magnitudes: low base means a smash does NOT kill at low percent, so
    // early-game exchanges are about building damage rather than fishing for an
    // instant kill. High growth then makes the same move lethal once the opponent
    // is worn down. Flip it (high base, low growth) and moves kill consistently at
    // every percent, which flattens the escalation arc that gives a match tension.
    // Calibrated against published Melee frame data, which uses exactly this shape.
    {/*su*/ 12, /*act*/ 4, /*tot*/ 39, // SMASH_SIDE
     /*rX*/ fxi(48), /*rY*/ fxi(-20), /*rad*/ fxi(30),
     /*dmg*/ fxi(15), /*bKB*/ fxi(10), /*kbG*/ fxi(105), /*ang*/ 38,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ true, /*landLag*/ 0},

    {/*su*/ 10, /*act*/ 5, /*tot*/ 38, // SMASH_UP
     /*rX*/ fxi(10), /*rY*/ fxi(-62), /*rad*/ fxi(32),
     /*dmg*/ fxi(14), /*bKB*/ fxi(8), /*kbG*/ fxi(112), /*ang*/ 88,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ true, /*landLag*/ 0},

    {/*su*/ 9, /*act*/ 4, /*tot*/ 36, // SMASH_DOWN
     /*rX*/ fxi(42), /*rY*/ fxi(4), /*rad*/ fxi(28),
     /*dmg*/ fxi(13), /*bKB*/ fxi(10), /*kbG*/ fxi(98), /*ang*/ 26,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ true, /*landLag*/ 0},

    // --- Aerials -------------------------------------------------------------
    // Neutral air: fast, wide, weak. The safety net you throw out when unsure.
    {/*su*/ 4, /*act*/ 6, /*tot*/ 28, // AIR_NEUTRAL
     /*rX*/ fxi(18), /*rY*/ fxi(-16), /*rad*/ fxi(34),
     /*dmg*/ fxi(8), /*bKB*/ fxi(14), /*kbG*/ fxi(60), /*ang*/ 45,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 7},

    {/*su*/ 6, /*act*/ 4, /*tot*/ 32, // AIR_FORWARD
     /*rX*/ fxi(42), /*rY*/ fxi(-10), /*rad*/ fxi(28),
     /*dmg*/ fxi(11), /*bKB*/ fxi(18), /*kbG*/ fxi(78), /*ang*/ 40,
     /*svX*/ 0, /*svY*/ fx_ratio(-8, 10), /*chg*/ false, /*landLag*/ 14},

    // Back air: stronger than forward, but you must be facing away. That tension
    // is what makes spacing and turnaround timing a skill.
    {/*su*/ 5, /*act*/ 3, /*tot*/ 28, // AIR_BACK
     /*rX*/ fxi(-40), /*rY*/ fxi(-14), /*rad*/ fxi(28),
     /*dmg*/ fxi(13), /*bKB*/ fxi(20), /*kbG*/ fxi(84), /*ang*/ 42,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 11},

    {/*su*/ 4, /*act*/ 4, /*tot*/ 26, // AIR_UP
     /*rX*/ fxi(6), /*rY*/ fxi(-58), /*rad*/ fxi(30),
     /*dmg*/ fxi(9), /*bKB*/ fxi(12), /*kbG*/ fxi(72), /*ang*/ 86,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 9},

    // Down air SPIKES: angle 270 drives the opponent straight down, which off-stage
    // is lethal and on-stage is nothing. High risk, high reward, positional.
    {/*su*/ 8, /*act*/ 4, /*tot*/ 34, // AIR_DOWN
     /*rX*/ fxi(14), /*rY*/ fxi(34), /*rad*/ fxi(28),
     /*dmg*/ fxi(12), /*bKB*/ fxi(18), /*kbG*/ fxi(70), /*ang*/ 270,
     /*svX*/ 0, /*svY*/ fx_ratio(15, 10), /*chg*/ false, /*landLag*/ 18},

    // Get-up attack. Weak and slow, with a wide radius covering both sides -- it
    // exists to discourage standing on top of a downed player, not to win exchanges.
    {/*su*/ 6, /*act*/ 4, /*tot*/ 34, // GETUP
     /*rX*/ 0, /*rY*/ fxi(-14), /*rad*/ fxi(42),
     /*dmg*/ fxi(7), /*bKB*/ fxi(16), /*kbG*/ fxi(50), /*ang*/ 45,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},

    // Ledge attack. Reaches onto the stage over the edge, which is exactly where
    // an edgeguarder stands -- that's its purpose, not raw damage.
    {/*su*/ 8, /*act*/ 4, /*tot*/ 38, // LEDGE
     /*rX*/ fxi(34), /*rY*/ fxi(-20), /*rad*/ fxi(34),
     /*dmg*/ fxi(8), /*bKB*/ fxi(16), /*kbG*/ fxi(56), /*ang*/ 40,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},
};

// --- Attack table: "Bruiser" -------------------------------------------------
// A heavy, slow, high-reward character. Deliberately built as a coherent opposite
// of Scout rather than a numeric tweak: every move is slower to start and longer
// to recover, but hits harder and reaches further. Combined with a heavier Body
// weight (which feeds the knockback formula's 200/(w+100) term) this produces a
// character that absorbs punishment and trades in single decisive hits.
//
// This table exists to prove the per-character structure is real -- the simulation
// reads whichever table the fighter points at and needs no knowledge of either.
constexpr AttackData kBruiserAttacks[ATK_COUNT] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, 0}, // ATK_NONE

    // Even the jab is committal: 5 frames instead of 3.
    {/*su*/ 5, /*act*/ 3, /*tot*/ 22,
     /*rX*/ fxi(38), /*rY*/ fxi(-18), /*rad*/ fxi(26),
     /*dmg*/ fxi(5), /*bKB*/ fxi(12), /*kbG*/ fxi(34), /*ang*/ 40,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},

    // --- Tilts ---------------------------------------------------------------
    {/*su*/ 8, /*act*/ 4, /*tot*/ 30, // TILT_SIDE
     /*rX*/ fxi(48), /*rY*/ fxi(-20), /*rad*/ fxi(28),
     /*dmg*/ fxi(12), /*bKB*/ fxi(16), /*kbG*/ fxi(66), /*ang*/ 32,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},

    {/*su*/ 7, /*act*/ 5, /*tot*/ 32, // TILT_UP
     /*rX*/ fxi(14), /*rY*/ fxi(-60), /*rad*/ fxi(30),
     /*dmg*/ fxi(11), /*bKB*/ fxi(14), /*kbG*/ fxi(72), /*ang*/ 84,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},

    {/*su*/ 8, /*act*/ 4, /*tot*/ 28, // TILT_DOWN
     /*rX*/ fxi(44), /*rY*/ fxi(2), /*rad*/ fxi(26),
     /*dmg*/ fxi(10), /*bKB*/ fxi(12), /*kbG*/ fxi(54), /*ang*/ 12,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},

    // --- Smashes: the payoff for being slow everywhere else. -----------------
    {/*su*/ 18, /*act*/ 4, /*tot*/ 54, // SMASH_SIDE
     /*rX*/ fxi(58), /*rY*/ fxi(-22), /*rad*/ fxi(36),
     /*dmg*/ fxi(22), /*bKB*/ fxi(14), /*kbG*/ fxi(118), /*ang*/ 38,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ true, /*landLag*/ 0},

    {/*su*/ 15, /*act*/ 5, /*tot*/ 50, // SMASH_UP
     /*rX*/ fxi(12), /*rY*/ fxi(-70), /*rad*/ fxi(38),
     /*dmg*/ fxi(20), /*bKB*/ fxi(12), /*kbG*/ fxi(124), /*ang*/ 88,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ true, /*landLag*/ 0},

    {/*su*/ 14, /*act*/ 5, /*tot*/ 48, // SMASH_DOWN
     /*rX*/ fxi(50), /*rY*/ fxi(4), /*rad*/ fxi(34),
     /*dmg*/ fxi(19), /*bKB*/ fxi(14), /*kbG*/ fxi(110), /*ang*/ 26,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ true, /*landLag*/ 0},

    // --- Aerials -------------------------------------------------------------
    {/*su*/ 7, /*act*/ 6, /*tot*/ 36, // AIR_NEUTRAL
     /*rX*/ fxi(20), /*rY*/ fxi(-18), /*rad*/ fxi(38),
     /*dmg*/ fxi(12), /*bKB*/ fxi(16), /*kbG*/ fxi(68), /*ang*/ 45,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 10},

    {/*su*/ 9, /*act*/ 4, /*tot*/ 40, // AIR_FORWARD
     /*rX*/ fxi(50), /*rY*/ fxi(-12), /*rad*/ fxi(32),
     /*dmg*/ fxi(16), /*bKB*/ fxi(20), /*kbG*/ fxi(88), /*ang*/ 40,
     /*svX*/ 0, /*svY*/ fx_ratio(-6, 10), /*chg*/ false, /*landLag*/ 20},

    {/*su*/ 8, /*act*/ 4, /*tot*/ 36, // AIR_BACK
     /*rX*/ fxi(-48), /*rY*/ fxi(-16), /*rad*/ fxi(32),
     /*dmg*/ fxi(18), /*bKB*/ fxi(22), /*kbG*/ fxi(94), /*ang*/ 42,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 16},

    {/*su*/ 7, /*act*/ 5, /*tot*/ 34, // AIR_UP
     /*rX*/ fxi(8), /*rY*/ fxi(-66), /*rad*/ fxi(34),
     /*dmg*/ fxi(13), /*bKB*/ fxi(14), /*kbG*/ fxi(80), /*ang*/ 86,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 13},

    {/*su*/ 12, /*act*/ 4, /*tot*/ 44, // AIR_DOWN (spike)
     /*rX*/ fxi(16), /*rY*/ fxi(38), /*rad*/ fxi(32),
     /*dmg*/ fxi(17), /*bKB*/ fxi(20), /*kbG*/ fxi(78), /*ang*/ 270,
     /*svX*/ 0, /*svY*/ fx_ratio(20, 10), /*chg*/ false, /*landLag*/ 26},

    {/*su*/ 9, /*act*/ 4, /*tot*/ 42, // GETUP
     /*rX*/ 0, /*rY*/ fxi(-16), /*rad*/ fxi(48),
     /*dmg*/ fxi(10), /*bKB*/ fxi(18), /*kbG*/ fxi(56), /*ang*/ 45,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},

    {/*su*/ 11, /*act*/ 4, /*tot*/ 46, // LEDGE
     /*rX*/ fxi(40), /*rY*/ fxi(-22), /*rad*/ fxi(38),
     /*dmg*/ fxi(12), /*bKB*/ fxi(18), /*kbG*/ fxi(62), /*ang*/ 40,
     /*svX*/ 0, /*svY*/ 0, /*chg*/ false, /*landLag*/ 0},
};

// --- Smash input & charge ----------------------------------------------------
// How a smash is distinguished from a tilt, mirroring the decomp's checkLStick:
//   attack pressed  AND  stick deflected past `deflection`  AND  the stick crossed
//   that threshold within the last `flickWindow` frames.
//
// The recency check is the whole trick, and it makes BOTH input methods work
// through one code path:
//   - flick the stick and press attack together -> crossing is fresh -> SMASH
//   - hold a direction, then press attack       -> crossing is stale -> TILT
// There is no separate flick detector; one timer distinguishes them.
struct SmashInput {
    int8_t deflection = 48; // stick magnitude required to register a direction
    int flickWindow = 4;    // frames since crossing that still counts as a flick

    // Fast-fall requires a fresh downward FLICK, not merely holding down --
    // ftCommon_CheckFallFast gates on timer_lstick_tilt_y being small, the same
    // recency test that separates a smash from a tilt. Holding down through a rise
    // should not auto-fast-fall the moment you start descending.
    int fastFallFlickWindow = 4;

    // Saturation cap for the stick-held timers. Any value at or above the flick
    // window means "not a flick", so the exact ceiling only needs to be large
    // enough to never wrap -- it is not a gameplay tunable.
    uint8_t stickTimerMax = 0xFE;

    // Charge: hold attack to store power, release (or cap out) to fire.
    int maxChargeFrames = 60;
    // Damage and knockback at full charge, as a multiple of the base value.
    fx fullChargeDamageMult = fx_ratio(14, 10);
    fx fullChargeKnockbackMult = fx_ratio(12, 10);
};

// --- Fighter -----------------------------------------------------------------
// One character's COMPLETE identity: movement, body, and its own attack table.
//
// This mirrors how Melee does it -- attributes hang off the fighter
// (fp->co_attrs.grav, fp->co_attrs.weight) and get passed into the physics
// functions rather than being globals. The simulation never reads a character
// constant directly; it reads them through the fighter the player selected.
//
// Adding a character is data only: append an entry to kFighters and give it an
// attack table. No new code, no branching on character type in the simulation.
struct Fighter {
    Body body;
    Jump jump;
    GroundMove ground;
    AirMove air;
    AirDodge airDodge;
    Landing landing;
    Knockdown knockdown;
    Ledge ledge;
    Shield shield;
    GroundEscape escape;

    // Per-character attacks. A heavy character's smash should not share numbers
    // with a fast one, so the whole table belongs to the fighter -- this pointer
    // is what makes movesets character-specific.
    const AttackData *attacks = nullptr;
};

// --- Stage geometry ----------------------------------------------------------
// The physical shape of one stage. Blast zones are what send you down a level.
struct StageLayout {
    fx groundY = fxi(420); // +Y is down
    fx platformLeft = fxi(240);
    fx platformRight = fxi(1040);
    fx blastLeft = fxi(-180);
    fx blastRight = fxi(1460);
    fx blastTop = fxi(-420);
    fx blastBottom = fxi(940);
    fx landSnapEpsilon = fxi(2); // tolerance on the "was above floor" test
};

// --- Arena -------------------------------------------------------------------
// The MVP structure: a FLAT POOL of stages. No levels, no depth, no summit.
//
//   - Each stage runs an independent match.
//   - Get KO'd past your stocks and you are EJECTED: routed to a different stage
//     that needs players. Your streak resets.
//   - Outlast everyone on your stage and you WIN the stage: a point, a streak
//     increment, and fresh opponents zone in to challenge you.
//
// Levels were dropped deliberately. A tiered structure needs a large concurrent
// population for its upper floors to be populated at all -- with 20 players
// online, whoever climbs highest ends up alone. A flat pool has no such failure
// mode: it works identically at 4 players or 400. Tiers are a later addition once
// population justifies them.
//
// Stages are still created on demand and closed when empty, so stage count tracks
// population rather than being pre-allocated.
struct Arena {
    int playersPerStage = 6; // a stage is "full" at this count
    int stocksPerStage = 1;  // knocked off once -> ejected to another stage

    // Below this population for this long, the stage is consolidated into another.
    // The grace period matters: don't yank someone the instant their opponent is
    // ejected, because new arrivals may be seconds away.
    int minPlayersPerStage = 2;
    int lonelyGraceFrames = 180; // 3 seconds at 60fps

    fx arrivalHeight = fxi(200); // drop-in height above the stage floor
    int arrivalInvulnFrames = 90;
};

// --- Scoring / leaderboard ---------------------------------------------------
// Net delta (KOs - knockoffs) is the base measure. A STREAK bonus sits on
// top, because the two reward different things:
//   net delta -> volume. Farmable by grinding a lot of fights.
//   streak    -> consecutive KOs without being knocked off. Much harder, and
//                invisible to net delta alone.
//
// There is no win bonus because the game is endless -- nobody "finishes". The
// streak bonus pays EVERY time the streak reaches a multiple of the threshold, so
// a 15-KO run earns it three times. Sustained dominance keeps paying rather than
// capping out at a single award.
//
// Kept as a modest kicker rather than a multiplier: if the bonus dwarfs base
// scoring, a single knock-off feels catastrophic and players start playing
// defensively to protect a streak instead of fighting.
struct Scoring {
    int pointsPerKO = 100;
    int pointsPerKnockoff = -40; // costs less than a KO earns: aggression is the goal
    int streakThreshold = 5;     // consecutive KOs without being knocked off
    int streakBonus = 250;       // awarded at every multiple of the threshold
};

// --- Roster ------------------------------------------------------------------
// The characters. The simulation indexes this by Player::charId and never reads a
// character constant any other way -- exactly as Melee passes fp->co_attrs into
// its physics functions rather than consulting globals.
//
// Adding a character is DATA ONLY: append an entry here plus an attack table.
// Nothing in sim.cpp branches on character identity.
enum CharId : uint8_t {
    CHAR_SCOUT = 0,
    CHAR_BRUISER,
    CHAR_COUNT,
};

constexpr Fighter kFighters[CHAR_COUNT] = {
    // --- Scout: light, fast, low commitment ----------------------------------
    Fighter{
        Body{/*halfWidth*/ fxi(16), /*height*/ fxi(46), /*weight*/ fxi(100)},
        Jump{},
        GroundMove{},
        AirMove{},
        AirDodge{},
        Landing{},
        Knockdown{},
        Ledge{},
        Shield{},
        GroundEscape{},
        kScoutAttacks,
    },

    // --- Bruiser: heavy, slow, high reward -----------------------------------
    // Every value here differs deliberately. The weight of 140 is the important
    // one: it feeds the knockback formula's 200/(w+100) term, so this character
    // genuinely resists launch rather than merely looking bigger. Slower gravity
    // and a weaker jump keep them grounded, which is where their reach pays off.
    Fighter{
        Body{/*halfWidth*/ fxi(21), /*height*/ fxi(58), /*weight*/ fxi(140)},
        Jump{/*jumpsquat*/ 6, /*full*/ fx_ratio(-56, 10), /*hop*/ fx_ratio(-32, 10),
             /*airJump*/ fx_ratio(-50, 10), /*maxAirJumps*/ 1},
        GroundMove{
            .walkInitVel = fx_ratio(3, 10),
            .walkAccel = fx_ratio(11, 100),
            .walkSpeed = fx_ratio(10, 10),
            .dashInitVel = fx_ratio(16, 10),
            .dashAccel = fx_ratio(15, 100),
            .dashSpeed = fx_ratio(25, 10),
            .dashFrames = 14,
            .runAccel = fx_ratio(13, 100),
            .runTaper = fx_ratio(12, 10),
            .runBrakeFrames = 18,
            .runBrakeFriction = fx_ratio(22, 100),
            .turnFrames = 9,
            .dashTurnFrames = 5,
            .maxHorizontal = fx_ratio(32, 10),
            .friction = fx_ratio(12, 100),
            .fastMultiplier = fxi(3),
        },
        AirMove{/*maxSpeed*/ fx_ratio(14, 10), /*accel*/ fx_ratio(8, 100),
                /*friction*/ fx_ratio(2, 100), /*gravity*/ fx_ratio(28, 100),
                /*termVel*/ fx_ratio(66, 10), /*fastFall*/ fx_ratio(105, 10)},
        AirDodge{/*speed*/ fx_ratio(55, 10), /*decay*/ fx_ratio(18, 100),
                 /*duration*/ 28, /*invuln*/ 20,
                 /*oneUsePerAirtime*/ true, /*consumesJumps*/ true,
                 /*helplessLandingLag*/ 20},
        Landing{/*aerialLag*/ 14, /*lcancelWindow*/ 7, /*lcancelDivisor*/ 2},
        // Heavier body: takes a harder slam to bounce, stays down longer, and gets
        // up more slowly. The tighter tech window is the cost of that durability.
        Knockdown{
            /*bounceThreshold*/ fx_ratio(48, 10),
            /*hardLandThreshold*/ fx_ratio(18, 10),
            /*techWindow*/ 16,
            /*techLockoutFrames*/ 48,
            /*techFrames*/ 32,
            /*techInvulnFrames*/ 20,
            /*bounceVelYMult*/ fx_ratio(-38, 100),
            /*bounceVelXMult*/ fx_ratio(45, 100),
            /*downWaitMinFrames*/ 16,
            /*downWaitMaxFrames*/ 100,
            /*getUpFrames*/ 28,
            /*getUpInvulnFrames*/ 12,
            /*rollFrames*/ 36,
            /*rollInvulnFrames*/ 22,
            /*rollSpeed*/ fx_ratio(30, 10),
            /*getUpAttackFrames*/ 42,
        },
        // Heavier: a slightly larger grab box (longer reach), but slower on every
        // ledge option. Durability paid for with recovery speed, same as knockdown.
        Ledge{
            /*grabReachX*/ fxi(30),
            /*grabReachUp*/ fxi(20),
            /*grabReachDown*/ fxi(62),
            /*requireFacing*/ true,
            /*hangOffsetX*/ fxi(18),
            /*hangOffsetY*/ fxi(42),
            /*hangFrames*/ 480,
            /*grabInvulnFrames*/ 30,
            /*cooldownFrames*/ 32,
            /*slowThreshold*/ fxi(100),
            /*climbQuickFrames*/ 32,
            /*climbSlowFrames*/ 56,
            /*rollQuickFrames*/ 40,
            /*rollSlowFrames*/ 64,
            /*rollInvulnFrames*/ 24,
            /*rollDistance*/ fxi(48),
            /*attackQuickFrames*/ 46,
            /*attackSlowFrames*/ 66,
            /*jumpFrames*/ 14,
            /*jumpVelY*/ fx_ratio(-52, 10),
            /*jumpVelX*/ fx_ratio(10, 10),
        },
        // Heavier: a bigger shield that drains slower, but far more release lag and
        // slower escapes. Durability traded against options -- the same axis the
        // knockdown and ledge values use.
        Shield{
            .maxHealth = fxi(78),
            .drainPerFrame = fx_ratio(12, 100),
            .regenPerFrame = fx_ratio(6, 100),
            .damageScale = fx_ratio(11, 10),
            .damageFlat = fx_ratio(28, 10),
            .startupFrames = 4,
            .releaseFrames = 16,
            .minHoldFrames = 5,
            .stunScale = fx_ratio(42, 100),
            .stunFlat = fx_ratio(20, 10),
            .stunMaxFrames = 30,
            .pushbackScale = fx_ratio(14, 100),
            .pushbackCap = fx_ratio(24, 10),
            .attackerPushScale = fx_ratio(10, 100),
            .attackerPushFlat = fx_ratio(5, 10),
        },
        GroundEscape{
            .rollFrames = 40,
            .rollInvulnStart = 5,
            .rollInvulnFrames = 18,
            .rollDistance = fxi(72),
            .dodgeFrames = 30,
            .dodgeInvulnStart = 3,
            .dodgeInvulnFrames = 16,
        },
        kBruiserAttacks,
    },
};

// --- Global instances --------------------------------------------------------
// Default fighter, used where no character has been chosen yet.
constexpr Fighter kFighter = kFighters[CHAR_SCOUT];
constexpr Knockback kKnockback{};
constexpr Hitstun kHitstun{};
constexpr Hitlag kHitlag{};
constexpr SDI kSDI{};
constexpr ShieldBreak kShieldBreak{};
constexpr DirectionalInfluence kDI{};
constexpr Respawn kRespawn{};
constexpr StickThresholds kStick{};
constexpr StageLayout kStage{};
constexpr Arena kArena{};
constexpr Scoring kScoring{};
constexpr SmashInput kSmash{};

// Storage capacity, not gameplay tuning -- these size fixed arrays. Stages are
// created on demand up to kMaxStages; the cap is a memory bound, not a design
// choice, and nothing pre-allocates up to it.
constexpr int kMaxStages = 128;
constexpr int kMaxPlayers = 8;          // per stage
constexpr int kMaxTrackedPlayers = 512; // arena-wide population bound
constexpr int kTicksPerSecond = 60;

} // namespace tf::config
