#include "skeleton.h"
#include "sim/config.h"

#include <cstddef>

namespace tf::pose {

namespace {

// Every state, in a fixed order so the editor can page through them predictably.
struct Entry {
    ActionState st;
    const char *label;
};

constexpr Entry kEntries[] = {
    {ActionState::Idle, "Idle"},
    {ActionState::Walk, "Walk"},
    {ActionState::Dash, "Dash"},
    {ActionState::Run, "Run"},
    {ActionState::RunBrake, "RunBrake"},
    {ActionState::Turn, "Turn"},
    {ActionState::Jumpsquat, "Jumpsquat"},
    {ActionState::Airborne, "Airborne"},
    {ActionState::Landing, "Landing"},
    {ActionState::AttackGround, "AttackGround"},
    {ActionState::AttackAir, "AttackAir"},
    {ActionState::AirDodge, "AirDodge"},
    {ActionState::FallHelpless, "FallHelpless"},
    {ActionState::Hitstun, "Hitstun"},
    {ActionState::Bounce, "Bounce"},
    {ActionState::DownWait, "DownWait"},
    {ActionState::GetUp, "GetUp"},
    {ActionState::GetUpRoll, "GetUpRoll"},
    {ActionState::GetUpAttack, "GetUpAttack"},
    {ActionState::Tech, "Tech"},
    {ActionState::LedgeHang, "LedgeHang"},
    {ActionState::LedgeClimb, "LedgeClimb"},
    {ActionState::LedgeRoll, "LedgeRoll"},
    {ActionState::LedgeAttack, "LedgeAttack"},
    {ActionState::LedgeJump, "LedgeJump"},
    {ActionState::ShieldOn, "ShieldOn"},
    {ActionState::Shield, "Shield"},
    {ActionState::ShieldOff, "ShieldOff"},
    {ActionState::ShieldStun, "ShieldStun"},
    {ActionState::ShieldBroken, "ShieldBroken"},
    {ActionState::Dizzy, "Dizzy"},
    {ActionState::RollForward, "RollForward"},
    {ActionState::RollBack, "RollBack"},
    {ActionState::SpotDodge, "SpotDodge"},
    {ActionState::Grabbing, "Grabbing"},
    {ActionState::GrabHold, "GrabHold"},
    {ActionState::Pummel, "Pummel"},
    {ActionState::Throwing, "Throwing"},
    {ActionState::GrabRelease, "GrabRelease"},
    {ActionState::Grabbed, "Grabbed"},
    {ActionState::Thrown, "Thrown"},
    {ActionState::Dead, "Dead"},
};

constexpr int kEntryCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

// Shorthand so a pose reads as one line:
//   P(lean, headY, shF, elF, shB, elB, hipF, knF, hipB, knB, crouch)
constexpr Pose P(float lean, float headY, float shF, float elF, float shB, float elB, float hipF,
                 float knF, float hipB, float knB, float crouch) {
    return Pose{lean, headY, shF, elF, shB, elB, hipF, knF, hipB, knB, crouch};
}

// Neutral standing pose, the baseline everything else is a variation on. Arms hang
// slightly out from the body, legs slightly apart.
constexpr Pose kStand = P(0, 0, 12, 8, -12, 8, 8, 4, -8, 4, 0);

// One-pose animation (a static hold).
constexpr Anim Hold(Pose a) { return Anim{a, a, 0, false, false}; }
// Two-pose animation interpolated over the state's own duration.
constexpr Anim Lerp(Pose a, Pose b) { return Anim{a, b, 0, false, true}; }
// Two-pose animation that ping-pongs on a fixed cycle (walk, run, idle sway).
constexpr Anim Cycle(Pose a, Pose b, int frames) { return Anim{a, b, frames, true, true}; }

// --- The pose table ---------------------------------------------------------
// Authored right-facing; the renderer mirrors for left. Angles are degrees from
// straight down, positive clockwise.
Anim kAnims[] = {
    // Idle: a slow breathing sway so a standing fighter is not perfectly static.
    Cycle(kStand, P(1, -0.01f, 14, 6, -10, 6, 8, 4, -8, 4, 0), 90),

    // Walk: legs alternate, arms counter-swing.
    Cycle(P(3, 0, 26, 14, -30, 14, 26, 10, -22, 18, 0.03f),
          P(3, 0, -22, 14, 30, 14, -24, 18, 28, 10, 0.03f), 24),

    // Dash: a hard forward lean with a wide leg split -- the burst.
    Cycle(P(16, 0, 46, 26, -52, 22, 40, 12, -36, 30, 0.08f),
          P(16, 0, -34, 26, 52, 22, -34, 30, 42, 12, 0.08f), 12),

    // Run: leaning further, faster cycle, arms driving.
    Cycle(P(22, -0.01f, 58, 32, -62, 28, 48, 14, -42, 36, 0.06f),
          P(22, -0.01f, -44, 32, 62, 28, -40, 36, 50, 14, 0.06f), 16),

    // RunBrake: heels dug in, torso pitched back against the momentum.
    Hold(P(-16, 0.01f, -36, 30, -44, 26, -34, 20, 30, 14, 0.22f)),

    // Turn: mid-pivot, weight crossing over.
    Hold(P(0, 0, 30, 24, -30, 24, 14, 26, -14, 26, 0.16f)),

    // Jumpsquat: deep compression, arms cocked to swing up.
    Lerp(P(6, -0.02f, 20, 14, -20, 14, 20, 20, -20, 20, 0.25f),
         P(4, -0.06f, -14, 20, -18, 20, 34, 52, -34, 52, 0.70f)),

    // Airborne: legs tucked, arms out for balance.
    Hold(P(4, 0, 44, 20, -46, 20, 30, 34, -26, 28, 0.10f)),

    // Landing: absorbing the impact, then rising back up.
    Lerp(P(8, -0.05f, 24, 22, -24, 22, 26, 46, -26, 46, 0.55f), kStand),

    // AttackGround: wind up, then extend the front arm through.
    Lerp(P(-8, 0, -20, 40, -34, 26, 12, 8, -16, 10, 0.10f),
         P(14, 0, 92, 4, -46, 30, 24, 6, -22, 12, 0.06f)),

    // AttackAir: a spinning kick -- front leg extends.
    Lerp(P(10, 0, 40, 24, -40, 24, 20, 40, -20, 20, 0.10f),
         P(18, 0, 30, 20, -60, 26, 86, 6, -30, 24, 0.05f)),

    // AirDodge: curled defensively, arms tucked in.
    Hold(P(24, 0.02f, 60, 60, -60, 60, 54, 60, -50, 56, 0.30f)),

    // FallHelpless: limp, arms trailing above -- reads as "no control".
    Cycle(P(-10, 0.01f, -40, 16, -50, 16, 16, 20, -12, 16, 0.05f),
          P(-14, 0.01f, -50, 16, -40, 16, 12, 16, -16, 20, 0.05f), 40),

    // Hitstun: thrown backwards, head snapped back, limbs flailing.
    Hold(P(-30, 0.02f, -60, 20, -70, 20, -24, 24, 26, 20, 0.08f)),

    // Bounce: folded from slamming into the ground.
    Hold(P(-40, 0.04f, -70, 40, -80, 40, -40, 50, 20, 46, 0.35f)),

    // DownWait: lying flat on the ground.
    Hold(P(-84, 0.10f, -80, 20, -92, 20, -80, 16, -96, 16, 0.85f)),

    // GetUp: pushing up off the floor back to standing.
    Lerp(P(-70, 0.08f, -70, 40, -84, 36, -66, 40, -84, 36, 0.75f), kStand),

    // GetUpRoll: tucked and rotating along the ground.
    Cycle(P(-60, 0.06f, -50, 70, -60, 70, -50, 70, -60, 70, 0.65f),
          P(-100, 0.06f, -90, 70, -100, 70, -90, 70, -100, 70, 0.65f), 12),

    // GetUpAttack: sweeping both legs out from the floor.
    Lerp(P(-70, 0.07f, -60, 40, -74, 36, -60, 44, -80, 40, 0.70f),
         P(-40, 0.04f, -30, 24, -50, 24, -96, 6, 60, 10, 0.45f)),

    // Tech: a controlled roll-out landing back on the feet.
    Lerp(P(-50, 0.05f, -40, 50, -52, 50, -40, 54, -52, 50, 0.55f), kStand),

    // LedgeHang: both arms up gripping the edge, body dangling.
    Hold(P(-4, 0.02f, 172, 10, 168, 10, 6, 20, -6, 16, 0.05f)),

    // LedgeClimb: pulling up and over.
    Lerp(P(-4, 0.02f, 172, 10, 168, 10, 6, 20, -6, 16, 0.05f),
         P(14, -0.02f, 60, 50, 74, 46, 42, 54, -18, 30, 0.35f)),

    // LedgeRoll: rolling in over the edge.
    Cycle(P(-40, 0.04f, -30, 70, -40, 70, -30, 70, -40, 70, 0.60f),
          P(-90, 0.04f, -80, 70, -90, 70, -80, 70, -90, 70, 0.60f), 14),

    // LedgeAttack: climbing up while swinging.
    Lerp(P(0, 0.01f, 150, 30, 160, 20, 20, 40, -14, 26, 0.20f),
         P(20, 0, 84, 6, -40, 30, 34, 12, -24, 18, 0.08f)),

    // LedgeJump: launching upward off the ledge.
    Lerp(P(-4, 0.02f, 172, 10, 168, 10, 6, 20, -6, 16, 0.05f),
         P(6, -0.02f, 20, 16, -22, 16, 26, 30, -22, 26, 0.10f)),

    // ShieldOn / Shield / ShieldOff: braced behind the bubble, arms forward.
    Lerp(kStand, P(10, 0.01f, 66, 54, 54, 50, 16, 16, -16, 16, 0.20f)),
    Hold(P(10, 0.01f, 70, 56, 58, 52, 16, 16, -16, 16, 0.22f)),
    Lerp(P(10, 0.01f, 70, 56, 58, 52, 16, 16, -16, 16, 0.22f), kStand),

    // ShieldStun: shoved back, arms still up.
    Hold(P(-6, 0.02f, 76, 58, 64, 54, -6, 20, 22, 18, 0.28f)),

    // ShieldBroken: flung upward, limbs splayed.
    Hold(P(-20, 0.03f, -80, 10, -100, 10, -30, 14, 30, 14, 0)),

    // Dizzy: slumped and swaying.
    Cycle(P(-12, 0.05f, -30, 30, -40, 26, 10, 26, -10, 22, 0.30f),
          P(12, 0.05f, 40, 26, 30, 30, -10, 22, 10, 26, 0.30f), 44),

    // RollForward / RollBack: tucked, rotating.
    Cycle(P(-50, 0.05f, -40, 70, -50, 70, -40, 70, -50, 70, 0.62f),
          P(-100, 0.05f, -90, 70, -100, 70, -90, 70, -100, 70, 0.62f), 14),
    Cycle(P(50, 0.05f, 40, 70, 50, 70, 40, 70, 50, 70, 0.62f),
          P(100, 0.05f, 90, 70, 100, 70, 90, 70, 100, 70, 0.62f), 14),

    // SpotDodge: compressed low in place.
    Lerp(P(4, -0.03f, 30, 40, -30, 40, 24, 40, -24, 40, 0.45f),
         P(2, -0.06f, 20, 56, -20, 56, 30, 66, -30, 66, 0.72f)),

    // Grabbing: both arms thrust forward.
    Lerp(P(-6, 0, 40, 30, -20, 20, 12, 8, -14, 10, 0.10f),
         P(16, 0, 96, 6, 84, 10, 26, 8, -22, 14, 0.06f)),

    // GrabHold: holding the opponent out in front.
    Hold(P(8, 0, 92, 20, 80, 24, 14, 10, -16, 12, 0.10f)),

    // Pummel: a short knee/strike into the held opponent.
    Lerp(P(8, 0, 92, 20, 80, 24, 14, 10, -16, 12, 0.10f),
         P(14, 0, 90, 18, 78, 22, 62, 70, -18, 14, 0.14f)),

    // Throwing: a full-body swing through.
    Lerp(P(-14, 0, 60, 30, 50, 30, 10, 10, -14, 12, 0.12f),
         P(26, 0, 130, 10, 116, 14, 34, 10, -28, 18, 0.06f)),

    // GrabRelease: staggering back a step.
    Lerp(P(-12, 0.01f, -20, 30, -30, 26, -14, 18, 20, 16, 0.20f), kStand),

    // Grabbed: held, feet off the ground, struggling.
    Cycle(P(-8, 0.02f, -50, 30, -60, 26, 20, 30, -16, 26, 0.10f),
          P(8, 0.02f, -60, 26, -50, 30, -16, 26, 20, 30, 0.10f), 8),

    // Thrown: launched, limbs trailing.
    Hold(P(-34, 0.03f, -70, 16, -80, 16, -30, 20, 26, 18, 0.05f)),

    // Dead: crumpled.
    Hold(P(-88, 0.12f, -84, 30, -96, 30, -84, 24, -100, 24, 0.90f)),
};

constexpr int kAnimCount = static_cast<int>(sizeof(kAnims) / sizeof(kAnims[0]));
static_assert(kAnimCount == kEntryCount,
              "every state in kEntries needs exactly one animation in kAnims");

int indexOf(ActionState st) {
    for (int i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].st == st) return i;
    }
    return 0;
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

Pose blend(const Pose &a, const Pose &b, float t) {
    Pose r;
    r.leanDeg = lerpf(a.leanDeg, b.leanDeg, t);
    r.headOffsetY = lerpf(a.headOffsetY, b.headOffsetY, t);
    r.shoulderFrontDeg = lerpf(a.shoulderFrontDeg, b.shoulderFrontDeg, t);
    r.elbowFrontDeg = lerpf(a.elbowFrontDeg, b.elbowFrontDeg, t);
    r.shoulderBackDeg = lerpf(a.shoulderBackDeg, b.shoulderBackDeg, t);
    r.elbowBackDeg = lerpf(a.elbowBackDeg, b.elbowBackDeg, t);
    r.hipFrontDeg = lerpf(a.hipFrontDeg, b.hipFrontDeg, t);
    r.kneeFrontDeg = lerpf(a.kneeFrontDeg, b.kneeFrontDeg, t);
    r.hipBackDeg = lerpf(a.hipBackDeg, b.hipBackDeg, t);
    r.kneeBackDeg = lerpf(a.kneeBackDeg, b.kneeBackDeg, t);
    r.crouch = lerpf(a.crouch, b.crouch, t);
    return r;
}

// How long a state naturally lasts, so a Lerp spans it rather than a magic number.
// Only used for presentation timing; a wrong value looks slightly off but cannot
// affect the simulation.
int naturalDuration(ActionState st) {
    const auto &F = config::kFighters[config::CHAR_SCOUT];
    switch (st) {
    case ActionState::Jumpsquat:    return F.jump.jumpsquatFrames;
    case ActionState::Landing:      return F.landing.aerialLagFrames;
    case ActionState::AttackGround: return config::kScoutAttacks[config::ATK_JAB].total;
    case ActionState::AttackAir:    return config::kScoutAttacks[config::ATK_AIR_NEUTRAL].total;
    case ActionState::AirDodge:     return F.airDodge.durationFrames;
    case ActionState::GetUp:        return F.knockdown.getUpFrames;
    case ActionState::GetUpAttack:  return F.knockdown.getUpAttackFrames;
    case ActionState::Tech:         return F.knockdown.techFrames;
    case ActionState::LedgeClimb:   return F.ledge.climbQuickFrames;
    case ActionState::LedgeAttack:  return F.ledge.attackQuickFrames;
    case ActionState::LedgeJump:    return F.ledge.jumpFrames;
    case ActionState::ShieldOn:     return F.shield.startupFrames;
    case ActionState::ShieldOff:    return F.shield.releaseFrames;
    case ActionState::SpotDodge:    return F.escape.dodgeFrames;
    case ActionState::Grabbing:     return F.grab.startupFrames + F.grab.activeFrames;
    case ActionState::Pummel:       return F.grab.pummelFrames;
    case ActionState::Throwing:     return config::kThrow.totalFrames;
    case ActionState::GrabRelease:  return F.grab.releaseFrames;
    default:                        return 20;
    }
}

} // namespace

Pose poseFor(ActionState st, int stateFrame, uint8_t attackId) {
    (void)attackId; // reserved: per-attack poses can key off this later
    const Anim &an = kAnims[indexOf(st)];
    if (!an.hasB) return an.a;

    const int span = an.frames > 0 ? an.frames : naturalDuration(st);
    if (span <= 1) return an.b;

    if (an.loop) {
        // Ping-pong, so a cycle reverses smoothly instead of snapping back.
        const int period = span * 2;
        int phase = stateFrame % period;
        if (phase < 0) phase += period;
        const float t = phase < span
                            ? static_cast<float>(phase) / static_cast<float>(span)
                            : 1.0f - static_cast<float>(phase - span) / static_cast<float>(span);
        return blend(an.a, an.b, t);
    }

    float t = static_cast<float>(stateFrame) / static_cast<float>(span);
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;
    return blend(an.a, an.b, t);
}

Anim &animFor(ActionState st) { return kAnims[indexOf(st)]; }
const char *stateLabel(ActionState st) { return kEntries[indexOf(st)].label; }
int stateCount() { return kEntryCount; }
ActionState stateByIndex(int i) {
    if (i < 0) i = 0;
    if (i >= kEntryCount) i = kEntryCount - 1;
    return kEntries[i].st;
}

} // namespace tf::pose
