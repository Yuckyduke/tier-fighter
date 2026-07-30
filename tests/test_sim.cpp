#include "sim/config.h"
#include "sim/state.h"
#include "sim/trig.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

// Tests for the deterministic simulation.
//
// The determinism tests matter more than the mechanics tests. A broken mechanic
// looks wrong immediately; broken determinism looks fine locally and then
// desyncs online, minutes into a match, with no useful stack trace. These
// assertions are how that gets caught at build time instead.

using namespace tf;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

void checkEq(int32_t got, int32_t want, const char* what) {
    ++g_checks;
    if (got != want) {
        std::printf("  FAIL: %s (got %d, want %d)\n", what, got, want);
        ++g_failures;
    }
}

void section(const char* name) { std::printf("\n[%s]\n", name); }

// --- Harness ----------------------------------------------------------------

GameState makeMatch(int playerCount) {
    GameState gs;
    for (int i = 0; i < playerCount; ++i) {
        Player& p = gs.players[i];
        p = Player{};
        p.active = true;
        p.stocks = 4;
        p.state = ActionState::Idle;
        p.y = gs.stage.groundY;
        p.x = gs.stage.platformLeft + fxi(200) + fxi(120) * i;
        p.facing = 1;
    }
    return gs;
}

struct Sequence {
    std::vector<Input> frames[kMaxPlayers];
};

// Run `frames` ticks, feeding scripted input and tracking prev-frame input so
// press-detection works exactly as it does in the real game loop.
void run(GameState& gs, const Sequence& seq, int frames) {
    Input prev[kMaxPlayers];
    for (int f = 0; f < frames; ++f) {
        Input cur[kMaxPlayers];
        for (int p = 0; p < kMaxPlayers; ++p) {
            const auto& track = seq.frames[p];
            cur[p] = (f < static_cast<int>(track.size())) ? track[f] : Input{};
        }
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    }
}

Input mk(uint16_t buttons = 0, int8_t sx = 0, int8_t sy = 0) {
    Input in;
    in.buttons = buttons;
    in.stickX = sx;
    in.stickY = sy;
    return in;
}

// --- Fixed-point ------------------------------------------------------------

void testFixedPoint() {
    section("fixed-point math");

    checkEq(fx_to_int(fxi(5)), 5, "fxi roundtrip");
    checkEq(fx_to_int(fx_mul(fxi(6), fxi(7))), 42, "6*7");
    checkEq(fx_to_int(fx_div(fxi(84), fxi(2))), 42, "84/2");
    checkEq(fx_ratio(1, 2), kFxHalf, "fx_ratio(1,2) == half");
    checkEq(fx_ratio(-3, 2), -fxi(1) - kFxHalf, "negative ratio");

    // Division by zero must be defined, not UB: a wrong number beats a desync.
    checkEq(fx_div(fxi(1), 0), 0, "div by zero is defined");

    // Negative-value handling is where naive fixed-point implementations break.
    checkEq(fx_to_int(fx_mul(fxi(-6), fxi(7))), -42, "negative multiply");
    checkEq(fx_abs(fxi(-9)), fxi(9), "abs");

    // fx_decay must land exactly on zero rather than crossing it, or players
    // jitter around a vanishing velocity forever.
    checkEq(fx_decay(fx_ratio(1, 100), fx_ratio(5, 100)), 0, "decay clamps at 0 (+)");
    checkEq(fx_decay(fx_ratio(-1, 100), fx_ratio(5, 100)), 0, "decay clamps at 0 (-)");
    check(fx_decay(fxi(10), fxi(1)) == fxi(9), "decay subtracts");

    // sqrt: exact squares must be exact, or normalized vectors drift.
    checkEq(fx_to_int(fx_sqrt(fxi(144))), 12, "sqrt(144)");
    checkEq(fx_to_int(fx_sqrt(fxi(10000))), 100, "sqrt(10000)");
    checkEq(fx_sqrt(0), 0, "sqrt(0)");
    checkEq(fx_sqrt(fxi(-5)), 0, "sqrt of negative is defined");

    // 3-4-5 triangle: the exact case the DI and air-dodge normalization rely on.
    const fx h = fx_sqrt(fx_mul(fxi(3), fxi(3)) + fx_mul(fxi(4), fxi(4)));
    checkEq(fx_to_int(h), 5, "sqrt(3^2+4^2) == 5");
}

void testTrig() {
    section("baked trig table");

    checkEq(fx_sin_deg(0), 0, "sin 0");
    checkEq(fx_sin_deg(90), kFxOne, "sin 90 == 1.0");
    checkEq(fx_sin_deg(180), 0, "sin 180");
    checkEq(fx_sin_deg(270), -kFxOne, "sin 270 == -1.0");
    checkEq(fx_cos_deg(0), kFxOne, "cos 0 == 1.0");
    checkEq(fx_cos_deg(90), 0, "cos 90");

    // Out-of-range angles must wrap, since DI can push an angle past 360 or
    // below 0 and an out-of-bounds table read would be a crash or garbage.
    checkEq(fx_sin_deg(450), kFxOne, "sin 450 wraps to sin 90");
    checkEq(fx_sin_deg(-90), -kFxOne, "sin -90 wraps");
    checkEq(fx_sin_deg(-270), kFxOne, "sin -270 wraps");

    // sin^2 + cos^2 == 1 across the circle, within fixed-point rounding.
    for (int deg = 0; deg < 360; deg += 15) {
        const fx s = fx_sin_deg(deg), c = fx_cos_deg(deg);
        const fx sum = fx_mul(s, s) + fx_mul(c, c);
        check(fx_abs(sum - kFxOne) < fx_ratio(1, 100), "sin^2+cos^2 ~= 1");
    }
}

// --- Determinism ------------------------------------------------------------

void testDeterminism() {
    section("determinism (rollback prerequisites)");

    Sequence seq;
    // A busy, varied input script: jumps, attacks, dodges, direction changes.
    for (int f = 0; f < 400; ++f) {
        uint16_t b = 0;
        if (f % 17 == 0) b |= BtnJump;
        if (f % 23 == 0) b |= BtnAttack;
        if (f % 61 == 0) b |= BtnShield;
        const int8_t sx = static_cast<int8_t>(((f / 7) % 3 - 1) * 80);
        const int8_t sy = static_cast<int8_t>(((f / 11) % 3 - 1) * 70);
        seq.frames[0].push_back(mk(b, sx, sy));

        uint16_t b2 = 0;
        if (f % 13 == 0) b2 |= BtnAttack;
        if (f % 29 == 0) b2 |= BtnJump;
        seq.frames[1].push_back(mk(b2, static_cast<int8_t>(-sx), sy));
    }

    // 1. Same inputs -> same checksum. The baseline requirement.
    GameState a = makeMatch(2), b = makeMatch(2);
    run(a, seq, 400);
    run(b, seq, 400);
    checkEq(static_cast<int32_t>(checksum(a)), static_cast<int32_t>(checksum(b)),
            "identical runs produce identical checksums");

    // 2. Snapshot / restore / re-simulate -- this IS rollback. Save at frame 100,
    //    run to 400, rewind to the snapshot, replay, and the result must match
    //    bit-for-bit. If this fails, rollback netcode is impossible.
    GameState c = makeMatch(2);
    run(c, seq, 100);
    GameState snapshot;
    std::memcpy(&snapshot, &c, sizeof(GameState));   // POD requirement in action

    Sequence tail;
    for (int p = 0; p < 2; ++p) {
        tail.frames[p].assign(seq.frames[p].begin() + 100, seq.frames[p].end());
    }
    run(c, tail, 300);
    const uint32_t direct = checksum(c);

    GameState restored;
    std::memcpy(&restored, &snapshot, sizeof(GameState));
    run(restored, tail, 300);
    checkEq(static_cast<int32_t>(checksum(restored)), static_cast<int32_t>(direct),
            "snapshot -> restore -> resimulate reproduces state exactly");

    // 3. Divergent input must actually change the checksum, otherwise the checksum
    //    is not observing enough state to detect a desync.
    //
    //    A SPAN of early frames is altered rather than a single late one. A lone
    //    frame is a fragile probe: if the player happens to be in a scripted state
    //    that ignores input (a ledge roll, a get-up, mid-attack), the altered frame
    //    is legitimately a no-op and the test fails while nothing is broken. Early
    //    frames, where both players are still interactive, test the real property --
    //    that the checksum observes enough state to notice divergence.
    Sequence altered = seq;
    for (int f = 20; f < 60; ++f) {
        altered.frames[0][f] = mk(BtnJump | BtnAttack, 99, -99);
    }
    GameState d = makeMatch(2);
    run(d, altered, 400);
    check(checksum(d) != checksum(a), "different inputs produce different checksums");

    // 4. GameState must stay memcpy-able. If someone adds a std::vector or a
    //    pointer to Player, rollback silently corrupts -- fail the build instead.
    check(std::is_trivially_copyable<GameState>::value,
          "GameState is trivially copyable (rollback requirement)");
    check(std::is_trivially_copyable<Player>::value,
          "Player is trivially copyable");
    check(std::is_trivially_copyable<Input>::value,
          "Input is trivially copyable (network requirement)");
}

// --- Movement mechanics -----------------------------------------------------

void testJumping() {
    section("jumping: short hop vs full jump");

    const auto& F = config::kFighter;

    // Full jump: hold jump through jumpsquat.
    GameState full = makeMatch(1);
    Sequence sFull;
    for (int f = 0; f < 10; ++f) sFull.frames[0].push_back(mk(BtnJump));
    // Sampled ON the takeoff frame. Jumpsquat occupies frames 1..N and velocity is
    // set on frame N; by frame N+1 gravity has already applied one tick, so
    // sampling later would read a value one gravity step short of the config.
    run(full, sFull, F.jump.jumpsquatFrames);
    const fx fullVel = full.players[0].selfVelY;

    // Short hop: release jump during jumpsquat.
    GameState hop = makeMatch(1);
    Sequence sHop;
    sHop.frames[0].push_back(mk(BtnJump));         // press
    for (int f = 1; f < 10; ++f) sHop.frames[0].push_back(mk());  // release
    run(hop, sHop, F.jump.jumpsquatFrames);
    const fx hopVel = hop.players[0].selfVelY;

    check(fullVel < 0, "full jump has upward velocity");
    check(hopVel < 0, "short hop has upward velocity");
    check(hopVel > fullVel, "short hop is weaker than full jump");
    checkEq(fullVel, F.jump.fullVelocity, "full jump uses configured velocity");
    checkEq(hopVel, F.jump.hopVelocity, "short hop uses configured velocity");

    // Jumpsquat must not be skippable -- it's the commitment window that makes
    // jumps readable and punishable.
    GameState js = makeMatch(1);
    Sequence sJs;
    for (int f = 0; f < 10; ++f) sJs.frames[0].push_back(mk(BtnJump));
    run(js, sJs, 1);
    check(js.players[0].state == ActionState::Jumpsquat,
          "jump enters jumpsquat, not airborne");
    check(js.players[0].selfVelY == 0, "no upward velocity during jumpsquat");
}

void testFastFall() {
    section("fast fall");

    GameState gs = makeMatch(1);
    Sequence seq;
    seq.frames[0].push_back(mk(BtnJump));
    for (int f = 1; f < 40; ++f) seq.frames[0].push_back(mk(BtnJump));
    run(gs, seq, 24);   // rise, then start descending

    check(gs.players[0].state == ActionState::Airborne, "airborne after jump");

    // Only allowed while already descending -- you can't fast fall upward.
    if (gs.players[0].selfVelY > 0) {
        const fx before = gs.players[0].selfVelY;
        Sequence ff;
        ff.frames[0].push_back(mk(0, 0, 99));   // hard stick down
        run(gs, ff, 1);
        check(gs.players[0].fastFalling, "hard stick down triggers fast fall");
        check(gs.players[0].selfVelY > before, "fast fall increases fall speed");
    }

    // Fast fall while rising must be rejected.
    GameState rising = makeMatch(1);
    Sequence r;
    r.frames[0].push_back(mk(BtnJump));
    for (int f = 1; f < 8; ++f) r.frames[0].push_back(mk(BtnJump, 0, 99));
    run(rising, r, 7);
    check(!rising.players[0].fastFalling, "cannot fast fall while rising");
}

// The headline test: wavedashing is EMERGENT. No wavedash code exists anywhere in
// the simulation. It falls out of three independent rules:
//   1. an air dodge sets velocity along the stick
//   2. landing zeroes only VERTICAL velocity
//   3. ground friction merely decays horizontal velocity
// If this passes, the momentum rules are right and other tech will emerge too.
void testOneAirDodgePerAirtime() {
    section("only one air dodge per airborne period");

    using namespace config;

    // REGRESSION: air dodges were unlimited. Seven in a single jump, and because
    // upward dodges add height, that gave infinite vertical recovery -- nobody
    // could ever be edge-guarded.
    //
    // Melee limits it STRUCTURALLY rather than with a counter: EscapeAir exits into
    // FallSpecial, a helpless state whose input handler has no air-dodge entry. We
    // mirror that with FallHelpless.
    GameState gs = makeMatch(1);
    {
        Player& p = gs.players[0];
        p.x = fxi(400);
        p.y = gs.stage.groundY;
        p.facing = 1;
    }

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](uint16_t b, int8_t sx, int8_t sy) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(b, sx, sy);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    for (int f = 0; f < 5; ++f) tick(BtnJump, 0, 0);
    check(gs.players[0].state == ActionState::Airborne, "airborne after jump");

    int dodges = 0;
    for (int f = 0; f < 240; ++f) {
        const bool press = (f % 30 == 0);
        tick(press ? BtnShield : 0, press ? 90 : 0, press ? -60 : 0);
        if (press && gs.players[0].state == ActionState::AirDodge) ++dodges;
        if (gs.players[0].state == ActionState::Idle && f > 10) break;
    }
    checkEq(dodges, 1, "exactly ONE air dodge per airborne period");
}

void testHelplessAfterAirDodge() {
    section("air dodge leaves you helpless");

    using namespace config;
    const auto& F = kFighters[CHAR_SCOUT];

    GameState gs = makeMatch(1);
    {
        Player& p = gs.players[0];
        p.state = ActionState::Airborne;
        p.x = fxi(600);
        p.y = gs.stage.groundY - fxi(400);
        p.airJumps = 0;
    }

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](uint16_t b, int8_t sx, int8_t sy) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(b, sx, sy);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    tick(BtnShield, 90, -40);
    check(gs.players[0].state == ActionState::AirDodge, "dodge started");

    // Dodging spends your jumps (their ftCommon_UseAllJumps).
    checkEq(gs.players[0].airJumps, F.jump.maxAirJumps,
            "air dodge consumes remaining jumps");

    // Ride out the dodge: it must land in the helpless state.
    for (int f = 0; f < F.airDodge.durationFrames + 4; ++f) tick(0, 0, 0);
    check(gs.players[0].state == ActionState::FallHelpless,
          "dodge exits into FallHelpless, not plain Airborne");

    // No jump, no attack, no second dodge out of helplessness.
    const fx yBefore = gs.players[0].y;
    tick(BtnJump, 0, 0);
    check(gs.players[0].state == ActionState::FallHelpless,
          "cannot jump out of helpless");
    tick(BtnAttack, 0, 0);
    check(gs.players[0].state == ActionState::FallHelpless,
          "cannot attack out of helpless");
    tick(BtnShield, 90, -40);
    check(gs.players[0].state == ActionState::FallHelpless,
          "cannot dodge again out of helpless");
    check(gs.players[0].y > yBefore, "still falling while helpless");

    // Drift MUST still work, or recovery becomes a coin flip.
    GameState drift = makeMatch(1);
    {
        Player& p = drift.players[0];
        p.state = ActionState::FallHelpless;
        p.x = fxi(600);
        p.y = drift.stage.groundY - fxi(400);
    }
    Sequence seq;
    for (int f = 0; f < 10; ++f) seq.frames[0].push_back(mk(0, 99));
    run(drift, seq, 10);
    check(drift.players[0].selfVelX > 0, "can still drift while helpless");
}

void testHelplessCanStillGrabLedge() {
    section("helpless players can still catch a ledge");

    using namespace config;

    // Otherwise an air dodge anywhere near the stage would be an unrecoverable
    // death sentence, which overshoots "commitment" into "trap".
    GameState gs = makeMatch(1);
    {
        Player& p = gs.players[0];
        p.state = ActionState::FallHelpless;
        p.x = gs.stage.platformRight + fxi(10);
        p.y = gs.stage.groundY + fxi(20);
        p.selfVelY = fx_ratio(10, 10);
        p.facing = -1;   // facing the stage, as the ledge rule requires
    }

    Sequence seq;
    for (int f = 0; f < 40; ++f) seq.frames[0].push_back(mk());
    run(gs, seq, 40);

    check(gs.players[0].state == ActionState::LedgeHang,
          "a helpless player can still grab the ledge");
}

void testHelplessLandingCostsMore() {
    section("landing while helpless costs extra lag");

    using namespace config;
    const auto& F = kFighters[CHAR_SCOUT];

    // The commitment is paid on the way out as well as the way in.
    check(F.airDodge.helplessLandingLag > F.landing.aerialLagFrames,
          "helpless landing lag exceeds a normal landing");

    GameState gs = makeMatch(1);
    {
        Player& p = gs.players[0];
        p.state = ActionState::FallHelpless;
        p.x = fxi(600);
        p.y = gs.stage.groundY - fxi(30);
        p.selfVelY = fx_ratio(20, 10);
    }

    Sequence seq;
    for (int f = 0; f < 60; ++f) seq.frames[0].push_back(mk());

    Input prev[tf::kMaxPlayers] = {};
    for (int f = 0; f < 60; ++f) {
        Input cur[tf::kMaxPlayers] = {};
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
        if (gs.players[0].state == ActionState::Landing) break;
    }
    check(gs.players[0].state == ActionState::Landing, "landed out of helpless");
    checkEq(gs.players[0].landingLag, F.airDodge.helplessLandingLag,
            "latched the helpless landing lag");
}

// --- Dash / Run split -------------------------------------------------------

// Drive ground movement with a scripted stick pattern and report the final state.
ActionState groundStateAfter(int8_t stickX, int frames, uint8_t charId) {
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.charId = charId;
    p.x = fxi(500);
    p.y = gs.stage.groundY;
    p.facing = 1;

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](int8_t sx) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(0, sx);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };
    tick(0);   // neutral first, so the next frame is a fresh crossing
    for (int f = 0; f < frames; ++f) tick(stickX);
    return gs.players[0].state;
}

void testDashCommitsToRun() {
    section("a held dash commits into a run");

    using namespace config;
    const auto& G = kFighters[CHAR_SCOUT].ground;

    // A hard flick starts a Dash. Holding it past dashFrames COMMITS you to a Run,
    // which is no longer interruptible -- that transition is what gives running a
    // cost and makes dash-dancing a distinct option rather than the only one.
    check(groundStateAfter(99, 1, CHAR_SCOUT) == ActionState::Dash,
          "a fresh hard flick starts a dash");
    check(groundStateAfter(99, G.dashFrames - 2, CHAR_SCOUT) == ActionState::Dash,
          "still dashing inside the dash window");
    check(groundStateAfter(99, G.dashFrames + 4, CHAR_SCOUT) == ActionState::Run,
          "holding past the dash window commits to a run");
}

void testDashDanceHoldsPosition() {
    section("dash-dancing holds position");

    using namespace config;

    // Dash-dancing works because Dash is INTERRUPTIBLE: ftCo_Dash_CheckInput
    // re-enters it on every fresh flick. The point of the technique is to threaten
    // both directions while staying put, so the test that matters is that repeated
    // reversals do not walk you across the stage.
    auto danceDrift = [](int cycles) {
        GameState gs = makeMatch(1);
        Player& p = gs.players[0];
        p.x = fxi(600);
        p.y = gs.stage.groundY;
        p.facing = 1;

        Input prev[tf::kMaxPlayers] = {};
        auto tick = [&](int8_t sx) {
            Input cur[tf::kMaxPlayers] = {};
            cur[0] = mk(0, sx);
            step(gs, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
        };
        const fx startX = p.x;
        for (int c = 0; c < cycles; ++c) {
            const int8_t dir = (c % 2 == 0) ? 99 : -99;
            tick(0);                                  // release
            for (int f = 0; f < 6; ++f) tick(dir);    // flick and hold briefly
        }
        return gs.players[0].x - startX;
    };

    // Drift must not ACCUMULATE. A one-time offset from the opening dash is fine
    // and expected; growth with cycle count would mean the dance walks you away.
    const fx d5 = danceDrift(5);
    const fx d20 = danceDrift(20);
    const fx d40 = danceDrift(40);

    check(fx_abs(d5) < fxi(60), "a short dance stays roughly in place");
    check(fx_abs(d40 - d20) < fxi(8),
          "drift does NOT accumulate with more cycles (one-time offset only)");
    check(fx_abs(d40) < fxi(60), "a long dance still holds position");
}

void testCannotDanceOutOfRun() {
    section("you cannot dash-dance out of a run");

    using namespace config;

    // Run has no dash re-entry, so reversing requires a brake. This asymmetry is
    // the entire reason committing to a run is a decision: a dash keeps your
    // options open, a run trades them for speed.
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.x = fxi(400);
    p.y = gs.stage.groundY;
    p.facing = 1;

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](int8_t sx) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(0, sx);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    tick(0);
    for (int f = 0; f < 20; ++f) tick(99);
    check(gs.players[0].state == ActionState::Run, "committed to a run");

    tick(0);
    tick(-99);
    check(gs.players[0].state == ActionState::RunBrake,
          "reversing out of a run requires braking, not an instant dash");
    check(gs.players[0].state != ActionState::Dash,
          "cannot re-enter a dash directly from a run");
}

void testTurnaroundCostsFrames() {
    section("turning around is a timed commitment");

    using namespace config;
    const auto& G = kFighters[CHAR_SCOUT].ground;

    // Ground reversal used to flip `facing` for free in a single frame, which
    // removed the risk from committing to a direction. Turn frames restore it.
    check(G.turnFrames > 0, "standing turn costs frames");
    check(G.dashTurnFrames > 0, "dash reversal costs frames");
    check(G.dashTurnFrames < G.turnFrames,
          "reversing out of a dash is quicker than a standing turn");

    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.x = fxi(500);
    p.y = gs.stage.groundY;
    p.facing = 1;

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](int8_t sx) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(0, sx);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    tick(0);
    for (int f = 0; f < 4; ++f) tick(99);   // dashing right
    check(gs.players[0].facing == 1, "facing right");

    tick(0);
    tick(-99);                              // flick left
    check(gs.players[0].state == ActionState::Turn, "entered a turn");
    checkEq(gs.players[0].facing, -1, "facing flips immediately");
    check(gs.players[0].groundActionFrames > 0, "turn frames are latched");
}

void testWalkAccelerates() {
    section("walking ramps up instead of snapping");

    using namespace config;
    const auto& G = kFighters[CHAR_SCOUT].ground;

    // Instant-on movement is the clearest sign a ground game is shallow. Walk now
    // starts at walkInitVel and accelerates toward walkSpeed.
    check(G.walkInitVel < G.walkSpeed, "initial walk velocity is below top speed");
    check(G.walkAccel > 0, "walk has an acceleration");

    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.x = fxi(500);
    p.y = gs.stage.groundY;
    p.facing = 1;

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](int8_t sx) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(0, sx);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    // A gentle deflection (below the dash threshold) walks.
    const int8_t soft = static_cast<int8_t>(kStick.deadzone + 6);
    tick(0);
    tick(soft);
    const fx firstFrame = gs.players[0].selfVelX;
    check(firstFrame > 0, "walking moves you");
    check(firstFrame < G.walkSpeed, "does NOT snap to top walk speed instantly");

    for (int f = 0; f < 20; ++f) tick(soft);
    check(fx_abs(gs.players[0].selfVelX) <= G.walkSpeed + fx_ratio(1, 100),
          "walk velocity converges on the cap");
    check(gs.players[0].selfVelX > firstFrame, "velocity increased over time");
}

void testRunAccelerationTapers() {
    section("run acceleration tapers toward top speed");

    using namespace config;

    // From ftCo_Run_Phys: acceleration scales by (1 - vel/target), so it is strong
    // at low speed and eases into the cap. Without the taper a run reaches top
    // speed abruptly and reads as a switch rather than a build-up.
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.x = fxi(300);
    p.y = gs.stage.groundY;
    p.facing = 1;

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&]() {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(0, 99);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };
    {
        Input cur[tf::kMaxPlayers] = {};
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    }

    // Get into a run, then sample acceleration early vs late.
    for (int f = 0; f < 14; ++f) tick();
    check(gs.players[0].state == ActionState::Run, "running");

    const fx v1 = gs.players[0].selfVelX;
    tick();
    const fx earlyDelta = gs.players[0].selfVelX - v1;

    for (int f = 0; f < 20; ++f) tick();
    const fx v2 = gs.players[0].selfVelX;
    tick();
    const fx lateDelta = gs.players[0].selfVelX - v2;

    check(earlyDelta >= 0, "accelerating early in the run");
    check(lateDelta <= earlyDelta,
          "acceleration tapers as top speed is approached");
}

void testGroundMoveIsPerCharacter() {
    section("ground movement values are per-character");

    using namespace config;
    const auto& scout = kFighters[CHAR_SCOUT].ground;
    const auto& bruiser = kFighters[CHAR_BRUISER].ground;

    // REGRESSION: Bruiser's GroundMove used a POSITIONAL initializer. Adding six
    // fields before dashSpeed silently shifted every value into the wrong slot --
    // walk speed landed in walkInitVel, dash speed in walkAccel, and so on. The
    // roster now uses designated initializers so field order cannot break it.
    check(bruiser.walkSpeed < scout.walkSpeed, "bruiser walks slower");
    check(bruiser.dashSpeed < scout.dashSpeed, "bruiser dashes slower");
    check(bruiser.dashInitVel < scout.dashInitVel, "bruiser has a weaker dash burst");
    check(bruiser.turnFrames > scout.turnFrames, "bruiser turns slower");
    check(bruiser.dashFrames > scout.dashFrames, "bruiser has a longer dash window");
    check(bruiser.friction > scout.friction, "bruiser has more grip");

    // Sanity: values must be plausible, not garbage from a shifted initializer.
    check(bruiser.walkSpeed > 0 && bruiser.walkSpeed < fxi(10),
          "bruiser walk speed is in a sane range");
    check(bruiser.dashSpeed > bruiser.walkSpeed,
          "bruiser dash is faster than its walk");
    check(scout.dashSpeed > scout.walkSpeed, "scout dash is faster than its walk");

    // Both characters must reach a run.
    check(groundStateAfter(99, scout.dashFrames + 4, CHAR_SCOUT) == ActionState::Run,
          "scout can reach a run");
    check(groundStateAfter(99, bruiser.dashFrames + 4, CHAR_BRUISER) == ActionState::Run,
          "bruiser can reach a run");
}

void testGroundFrictionIsNotIcy() {
    section("ground friction stops you crisply without killing the wavedash");

    using namespace config;
    const auto& F = kFighters[CHAR_SCOUT];

    // These two values are in a tug-of-war and must be tuned TOGETHER.
    //
    // Friction was originally 0.08, which took 43 frames and 70px to stop a dash --
    // three quarters of a second of coasting, which played like ice. But friction is
    // also the only thing decaying a wavedash, so raising it alone shrank wavedashes
    // from 81px to 27px and made the technique pointless.
    //
    // This test pins BOTH outcomes so a future change to either value cannot quietly
    // reintroduce the ice or flatten the wavedash.

    // --- 1. Stopping from a dash must be crisp ------------------------------
    GameState stop = makeMatch(1);
    {
        Player& p = stop.players[0];
        p.x = fxi(500);
        p.y = stop.stage.groundY;
        p.facing = 1;
    }
    Sequence toDash;
    for (int f = 0; f < 10; ++f) toDash.frames[0].push_back(mk(0, 99));
    run(stop, toDash, 10);

    const fx dashStartX = stop.players[0].x;
    check(stop.players[0].selfVelX > 0, "reached dash speed");

    int stopFrames = 0;
    {
        Input prev[tf::kMaxPlayers] = {};
        for (int f = 0; f < 300; ++f) {
            Input cur[tf::kMaxPlayers] = {};
            step(stop, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
            ++stopFrames;
            if (stop.players[0].selfVelX == 0) break;
        }
    }
    const fx slid = stop.players[0].x - dashStartX;

    check(stopFrames <= 22, "a dash stops within ~22 frames (not 43 -- no ice)");
    check(slid < F.body.halfWidth * 4, "slides under two body widths after a dash");

    // --- 2. The wavedash must remain worthwhile -----------------------------
    GameState wd = makeMatch(1);
    {
        Player& p = wd.players[0];
        p.x = fxi(500);
        p.y = wd.stage.groundY;
        p.facing = 1;
    }
    const fx wdStartX = wd.players[0].x;

    Sequence seq;
    for (int f = 0; f < 5; ++f) seq.frames[0].push_back(mk(BtnJump));
    seq.frames[0].push_back(mk(BtnShield, 90, 45));      // shallow down-forward
    for (int f = 6; f < 120; ++f) seq.frames[0].push_back(mk());
    run(wd, seq, 120);

    const fx wdSlide = wd.players[0].x - wdStartX;
    check(wdSlide > F.body.halfWidth * 3,
          "a wavedash still covers over 1.5 body widths");
    check(wdSlide > slid, "a wavedash travels FURTHER than a plain dash stop");

    // --- 3. Friction must be two-tier ---------------------------------------
    // ft_80084F3C multiplies friction when moving faster than walkSpeed. Both
    // tiers have to exist: a single flat value cannot brake a dash crisply AND
    // preserve a wavedash glide.
    check(F.ground.fastMultiplier > kFxOne,
          "fast-tier friction is stronger than the base tier");
    check(F.airDodge.speed > F.ground.dashSpeed,
          "air dodge launches faster than a dash, so wavedashes outrun walking");

    // --- 4. The GLIDE, not just the distance --------------------------------
    // This is the check that a distance-only test misses. A flat high friction
    // produced MORE total wavedash distance than the original, yet felt worse,
    // because the slide decayed uniformly and stopped abruptly. What reads as a
    // wavedash is the long low-speed tail.
    //
    // So: assert the slide spends real time below walking speed, still moving.
    // That tail is the mechanic; total distance is a side effect.
    GameState glide = makeMatch(1);
    {
        Player& p = glide.players[0];
        p.x = fxi(500);
        p.y = glide.stage.groundY;
        p.facing = 1;
    }
    Sequence g;
    for (int f = 0; f < 5; ++f) g.frames[0].push_back(mk(BtnJump));
    g.frames[0].push_back(mk(BtnShield, 90, 45));
    for (int f = 6; f < 160; ++f) g.frames[0].push_back(mk());

    Input gprev[tf::kMaxPlayers] = {};
    int slowGlideFrames = 0;
    bool everFast = false;
    for (int f = 0; f < 160; ++f) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = (f < static_cast<int>(g.frames[0].size())) ? g.frames[0][f] : mk();
        step(glide, cur, gprev);
        std::memcpy(gprev, cur, sizeof(cur));

        const Player& p = glide.players[0];
        const bool grounded = (p.state == ActionState::Idle ||
                               p.state == ActionState::Landing ||
                               p.state == ActionState::Walk ||
                               p.state == ActionState::Dash);
        if (!grounded) continue;
        const fx sp = fx_abs(p.selfVelX);
        if (sp > F.ground.walkSpeed) everFast = true;
        if (sp > 0 && sp <= F.ground.walkSpeed) ++slowGlideFrames;
        if (sp == 0 && everFast) break;
    }
    check(everFast, "the wavedash starts above walking speed");
    check(slowGlideFrames >= 6,
          "the slide spends real frames gliding BELOW walking speed (the tail)");
}

void testWavedashEmerges() {
    section("wavedash (emergent, not implemented)");

    GameState gs = makeMatch(1);
    const fx startX = gs.players[0].x;

    Sequence seq;
    seq.frames[0].push_back(mk(BtnJump));                 // jumpsquat
    for (int f = 1; f < 5; ++f) seq.frames[0].push_back(mk(BtnJump));
    // Airborne: dodge shallow down-and-forward.
    seq.frames[0].push_back(mk(BtnShield, 90, 45));
    for (int f = 6; f < 60; ++f) seq.frames[0].push_back(mk());

    run(gs, seq, 6);
    check(gs.players[0].state == ActionState::AirDodge, "air dodge started");
    check(gs.players[0].selfVelX > 0, "dodge imparts horizontal velocity");
    check(gs.players[0].selfVelY > 0, "dodge angled downward");

    // Land, then confirm horizontal momentum survived the landing.
    fx velAtLanding = 0;
    bool landed = false;
    for (int f = 0; f < 40 && !landed; ++f) {
        Sequence one;
        one.frames[0].push_back(mk());
        run(gs, one, 1);
        if (gs.players[0].state == ActionState::Landing ||
            gs.players[0].state == ActionState::Idle) {
            landed = true;
            velAtLanding = gs.players[0].selfVelX;
        }
    }

    check(landed, "player landed after air dodge");
    check(velAtLanding > 0, "horizontal momentum SURVIVES landing (the wavedash)");
    check(gs.players[0].x > startX, "player slid forward from the wavedash");

    // And it must actually decay to a stop rather than sliding forever.
    Sequence idle;
    for (int f = 0; f < 120; ++f) idle.frames[0].push_back(mk());
    run(gs, idle, 120);
    checkEq(gs.players[0].selfVelX, 0, "friction eventually stops the slide");
}

// Drop an aerial into the ground and measure the resulting landing lag.
//
// Shield is pressed ONLY while still in AttackAir and close to the ground -- that
// is precisely what L-cancelling is. Spraying shield indiscriminately would put the
// player into an AIR DODGE the moment the aerial ends, and they would never land.
int measureLandingLag(bool lcancel) {
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.state = ActionState::Airborne;
    p.y = gs.stage.groundY - fxi(60);
    p.selfVelY = fx_ratio(20, 10);   // already descending

    Input prev[kMaxPlayers] = {};
    auto tick = [&](uint16_t buttons) {
        Input cur[kMaxPlayers] = {};
        cur[0] = mk(buttons);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    tick(BtnAttack);   // neutral aerial

    for (int f = 0; f < 200; ++f) {
        if (gs.players[0].state == ActionState::Landing) break;
        const bool nearGround =
            (gs.stage.groundY - gs.players[0].y) < fxi(14);
        const bool inAerial = gs.players[0].state == ActionState::AttackAir;
        tick((lcancel && nearGround && inAerial) ? BtnShield : 0);
    }
    if (gs.players[0].state != ActionState::Landing) return -1;

    int lag = 0;
    for (int f = 0; f < 80; ++f) {
        tick(0);
        ++lag;
        if (gs.players[0].state == ActionState::Idle) break;
    }
    return lag;
}

void testLCancel() {
    section("L-cancel");

    const auto& F = config::kFighter;

    const int normal = measureLandingLag(false);
    const int cancelled = measureLandingLag(true);

    // measureLandingLag throws a NEUTRAL aerial, which now carries its OWN landing
    // lag rather than sharing the character default. Melee has five separate values
    // (landingairn_lag, landingairf_lag, ...) so each aerial is differently safe to
    // land with -- asserting against the character default would test the old
    // shared-number behaviour that was just removed.
    const int nairLag = config::kScoutAttacks[config::ATK_AIR_NEUTRAL].landingLag;

    check(normal > 0, "measured normal landing lag");
    check(cancelled > 0, "measured L-cancelled landing lag");
    checkEq(normal, nairLag, "normal lag matches the aerial's own value");
    checkEq(cancelled, nairLag / F.landing.lcancelDivisor,
            "L-cancelled lag is halved");
    check(cancelled < normal, "L-cancel reduces landing lag");
}

// --- Attack matrix ----------------------------------------------------------

// Drive one attack input from a known state and report which move came out.
uint8_t attackFrom(bool airborne, int8_t sx, int8_t sy, int holdFrames) {
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    if (airborne) {
        p.state = ActionState::Airborne;
        p.y = gs.stage.groundY - fxi(120);
    }
    p.facing = 1;

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](const Input& i0) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = i0;
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    // Hold the direction WITHOUT attacking for holdFrames. This is what ages the
    // stick timer: 0 means the crossing is fresh (a flick), several frames means
    // it is stale (a hold).
    for (int f = 0; f < holdFrames; ++f) tick(mk(0, sx, sy));
    tick(mk(BtnAttack, sx, sy));
    return gs.players[0].attackId;
}

void testGroundAttackMatrix() {
    section("ground attack matrix: jab / tilts / smashes");

    using namespace config;

    // No direction -> jab.
    checkEq(attackFrom(false, 0, 0, 0), ATK_JAB, "no direction -> jab");

    // FLICK (fresh crossing, hold 0 frames) -> smash.
    checkEq(attackFrom(false, 99, 0, 0), ATK_SMASH_SIDE, "flick side -> side smash");
    checkEq(attackFrom(false, 0, -99, 0), ATK_SMASH_UP, "flick up -> up smash");
    checkEq(attackFrom(false, 0, 99, 0), ATK_SMASH_DOWN, "flick down -> down smash");
    checkEq(attackFrom(false, -99, 0, 0), ATK_SMASH_SIDE, "flick left -> side smash");

    // HOLD (stale crossing) -> tilt. Same stick position, different history: this
    // is the entire distinction, and it is what lets both input styles coexist.
    const int stale = kSmash.flickWindow + 3;
    checkEq(attackFrom(false, 99, 0, stale), ATK_TILT_SIDE, "held side -> side tilt");
    checkEq(attackFrom(false, 0, -99, stale), ATK_TILT_UP, "held up -> up tilt");
    checkEq(attackFrom(false, 0, 99, stale), ATK_TILT_DOWN, "held down -> down tilt");

    // Boundary: just inside the window is still a flick, just outside is not.
    //
    // Uses a MID deflection (past kSmash.deflection but below kStick.hard) so the
    // flick window is tested in isolation. A full 99 deflection held for a few
    // frames now enters a DASH, and entering a dash deliberately saturates the
    // stick timer -- so the later attack would come out of the dash with a stale
    // timer and read as a tilt. That is correct behaviour (in Melee, attacking out
    // of a dash is a dash attack, not a side smash), it just makes 99 the wrong
    // probe for this particular boundary.
    const int8_t midDeflect =
        static_cast<int8_t>((kSmash.deflection + kStick.hard) / 2);
    check(midDeflect >= kSmash.deflection, "probe registers as a direction");
    check(midDeflect < kStick.hard, "probe is below the dash threshold");

    checkEq(attackFrom(false, midDeflect, 0, kSmash.flickWindow - 1),
            ATK_SMASH_SIDE, "last frame of the flick window still smashes");
    checkEq(attackFrom(false, midDeflect, 0, kSmash.flickWindow),
            ATK_TILT_SIDE, "one frame past the window tilts");

    // And the interaction that replaced it: a hard flick WITHOUT attack starts a
    // dash, so attacking afterwards is no longer a smash. Flick and attack on the
    // SAME frame still smashes -- asserted above.
    checkEq(attackFrom(false, 99, 0, 3), ATK_TILT_SIDE,
            "attacking after a hard flick comes out of the dash, not a smash");

    // Below the deflection threshold the stick does not register a direction.
    checkEq(attackFrom(false, static_cast<int8_t>(kSmash.deflection - 5), 0, 0),
            ATK_JAB, "sub-threshold deflection -> jab");
}

void testAirAttackMatrix() {
    section("air attack matrix: nair / fair / bair / uair / dair");

    using namespace config;

    checkEq(attackFrom(true, 0, 0, 0), ATK_AIR_NEUTRAL, "no direction -> nair");
    checkEq(attackFrom(true, 0, -99, 0), ATK_AIR_UP, "up -> uair");
    checkEq(attackFrom(true, 0, 99, 0), ATK_AIR_DOWN, "down -> dair");

    // Forward vs back is relative to FACING, which is why bair can hit harder:
    // landing it requires turning around or preserving momentum.
    checkEq(attackFrom(true, 99, 0, 0), ATK_AIR_FORWARD,
            "stick toward facing -> fair");
    checkEq(attackFrom(true, -99, 0, 0), ATK_AIR_BACK,
            "stick away from facing -> bair");

    // Aerials have no smash tier, so holding vs flicking must not change the move.
    checkEq(attackFrom(true, 99, 0, 10), ATK_AIR_FORWARD,
            "held direction still gives fair (no air smashes)");

    // Back air reaches behind the attacker.
    check(kScoutAttacks[ATK_AIR_BACK].reachX < 0, "bair hitbox is behind the attacker");
    // Down air spikes.
    checkEq(kScoutAttacks[ATK_AIR_DOWN].angleDeg, 270, "dair angle points straight down");
}

void testSmashCharge() {
    section("smash charging");

    using namespace config;

    // Hold attack through the charge point and the move must WAIT rather than
    // advancing -- the wind-up is held so the opponent can read it.
    GameState gs = makeMatch(1);
    gs.players[0].facing = 1;

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](const Input& i0) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = i0;
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    tick(mk(BtnAttack, 99, 0));   // flick + attack -> side smash
    checkEq(gs.players[0].attackId, ATK_SMASH_SIDE, "side smash started");

    // Keep holding attack. Once at the charge point it should stall and bank frames.
    for (int f = 0; f < 30; ++f) tick(mk(BtnAttack, 99, 0));
    check(gs.players[0].charging, "smash is charging");
    check(gs.players[0].chargeFrames > 0, "charge frames banked");
    const uint16_t heldFrame = gs.players[0].attackFrame;

    for (int f = 0; f < 5; ++f) tick(mk(BtnAttack, 99, 0));
    checkEq(gs.players[0].attackFrame, heldFrame,
            "attackFrame does NOT advance while charging");

    // Charge must saturate rather than growing without bound. Sampled while STILL
    // charging: once the cap is reached the move fires automatically and the state
    // resets, so checking afterwards would read 0.
    uint8_t peakCharge = 0;
    for (int f = 0; f < 200; ++f) {
        tick(mk(BtnAttack, 99, 0));
        if (gs.players[0].charging && gs.players[0].chargeFrames > peakCharge) {
            peakCharge = gs.players[0].chargeFrames;
        }
    }
    checkEq(peakCharge, kSmash.maxChargeFrames,
            "charge saturates at the configured maximum");

    // Release: the hitbox comes out and the move completes.
    for (int f = 0; f < 60; ++f) tick(mk(0, 0, 0));
    check(!gs.players[0].charging, "charge released");

    // A charged smash must hit harder than an uncharged one.
    auto hitDamage = [](int chargeHoldFrames) {
        GameState g = makeMatch(2);
        g.players[0].x = fxi(500);
        g.players[0].facing = 1;
        g.players[1].x = fxi(540);
        g.players[1].damage = 0;

        Input pv[tf::kMaxPlayers] = {};
        auto t = [&](const Input& a) {
            Input cur[tf::kMaxPlayers] = {};
            cur[0] = a;
            step(g, cur, pv);
            std::memcpy(pv, cur, sizeof(cur));
        };
        t(mk(BtnAttack, 99, 0));
        for (int f = 0; f < chargeHoldFrames; ++f) t(mk(BtnAttack, 99, 0));
        for (int f = 0; f < 60; ++f) t(mk(0, 0, 0));
        return fx_to_int(g.players[1].damage);
    };

    const int uncharged = hitDamage(0);
    const int charged = hitDamage(kSmash.maxChargeFrames + 10);
    check(uncharged > 0, "uncharged smash connects");
    check(charged > uncharged, "charged smash deals more damage");
}

void testAttackProfiles() {
    section("attack profiles are internally coherent");

    using namespace config;

    // Smashes must be slower AND stronger than tilts, or there is no reason to
    // ever risk one.
    check(kScoutAttacks[ATK_SMASH_SIDE].startup > kScoutAttacks[ATK_TILT_SIDE].startup,
          "side smash is slower than side tilt");
    check(kScoutAttacks[ATK_SMASH_SIDE].damage > kScoutAttacks[ATK_TILT_SIDE].damage,
          "side smash hits harder than side tilt");
    check(kScoutAttacks[ATK_SMASH_SIDE].total > kScoutAttacks[ATK_TILT_SIDE].total,
          "side smash has a longer punish window on whiff");

    // Jab is the fastest thing available -- it is the panic button.
    for (int i = ATK_TILT_SIDE; i < ATK_COUNT; ++i) {
        check(kScoutAttacks[ATK_JAB].startup <= kScoutAttacks[i].startup,
              "jab is the fastest attack");
    }

    // Only smashes charge.
    for (int i = 1; i < ATK_COUNT; ++i) {
        const bool isSmash = (i == ATK_SMASH_SIDE || i == ATK_SMASH_UP ||
                              i == ATK_SMASH_DOWN);
        checkEq(kScoutAttacks[i].chargeable ? 1 : 0, isSmash ? 1 : 0,
                "only smash attacks are chargeable");
    }

    // Every real attack must have a live window and a total that contains it.
    for (int i = 1; i < ATK_COUNT; ++i) {
        check(kScoutAttacks[i].active > 0, "attack has active frames");
        check(kScoutAttacks[i].radius > 0, "attack has a hitbox radius");
        check(kScoutAttacks[i].damage > 0, "attack deals damage");
        check(kScoutAttacks[i].total >= kScoutAttacks[i].startup + kScoutAttacks[i].active,
              "total animation contains the active window");
    }

    // Smashes use LOW base knockback with HIGH growth. That shape is what makes
    // damage escalate over a match instead of every hit killing equally.
    check(kScoutAttacks[ATK_SMASH_SIDE].knockbackGrowth >
              kScoutAttacks[ATK_SMASH_SIDE].baseKnockback,
          "smash growth exceeds base knockback (escalating, not flat)");
}


void testKnockbackFormula() {
    section("knockback scaling");

    GameState gs = makeMatch(2);
    // Put them adjacent so a jab connects.
    gs.players[0].x = fxi(500);
    gs.players[1].x = fxi(520);
    gs.players[0].facing = 1;

    Sequence seq;
    seq.frames[0].push_back(mk(BtnAttack));
    for (int f = 1; f < 12; ++f) seq.frames[0].push_back(mk());

    run(gs, seq, 6);   // past startup, into active frames

    check(gs.players[1].damage > 0, "attack dealt damage");
    check(gs.players[1].state == ActionState::Hitstun, "victim is in hitstun");
    check(gs.players[1].hitstunFrames > 0, "hitstun frames assigned");
    // Knockback must land in kbVel, not selfVel -- that separation is what lets
    // the victim drift while launched.
    check(gs.players[1].kbVelX != 0 || gs.players[1].kbVelY != 0,
          "knockback stored in kbVel");

    const fx lowPercentKb = fx_abs(gs.players[1].kbVelX) + fx_abs(gs.players[1].kbVelY);

    // Same attack against a high-damage victim must launch much further. This is
    // the superlinear damage scaling that makes matches escalate.
    GameState high = makeMatch(2);
    high.players[0].x = fxi(500);
    high.players[1].x = fxi(520);
    high.players[0].facing = 1;
    high.players[1].damage = fxi(120);
    run(high, seq, 6);

    const fx highPercentKb =
        fx_abs(high.players[1].kbVelX) + fx_abs(high.players[1].kbVelY);

    check(highPercentKb > lowPercentKb, "higher damage -> more knockback");
    check(high.players[1].hitstunFrames >= gs.players[1].hitstunFrames,
          "higher damage -> at least as much hitstun");

    // Attacks must not hit the same target twice in one swing.
    checkEq(fx_to_int(gs.players[1].damage), fx_to_int(config::kScoutAttacks[config::ATK_JAB].damage),
            "one hit per swing, not repeated per active frame");
}

void testAirDriftDuringHitstun() {
    section("air drift during hitstun (velocity decomposition)");

    // The reason selfVel and kbVel are separate. A launched player holding
    // *away* from their launch must accumulate opposing self-velocity while
    // knockback decays independently.
    GameState gs = makeMatch(2);
    gs.players[0].x = fxi(500);
    gs.players[1].x = fxi(520);
    gs.players[0].facing = 1;

    Sequence hit;
    hit.frames[0].push_back(mk(BtnAttack));
    run(gs, hit, 6);
    check(gs.players[1].state == ActionState::Hitstun, "victim launched");

    const fx kbBefore = fx_abs(gs.players[1].kbVelX);

    // Victim holds backward (away from launch direction) for several frames.
    Sequence drift;
    for (int f = 0; f < 8; ++f) {
        drift.frames[0].push_back(mk());
        drift.frames[1].push_back(mk(0, -99, 0));
    }
    run(gs, drift, 8);

    check(gs.players[1].selfVelX < 0, "victim accumulated drift against knockback");
    check(fx_abs(gs.players[1].kbVelX) < kbBefore, "knockback decayed independently");
}

void testInvulnerability() {
    section("invulnerability");

    GameState gs = makeMatch(2);
    gs.players[0].x = fxi(500);
    gs.players[1].x = fxi(520);
    gs.players[0].facing = 1;
    gs.players[1].invulnFrames = 60;

    Sequence seq;
    seq.frames[0].push_back(mk(BtnAttack));
    run(gs, seq, 6);

    checkEq(fx_to_int(gs.players[1].damage), 0, "invulnerable target takes no damage");
    check(gs.players[1].state != ActionState::Hitstun, "invulnerable target not stunned");
}

void testHitboxDistanceNoOverflow() {
    section("hitbox distance test does not overflow (regression)");

    // REGRESSION: the overlap test used fx_mul, which returns int32_t. Squaring a
    // distance in Q16.16 overflows past ~181 world units and wraps NEGATIVE, so
    // "distance^2 <= radius^2" reported a hit from across the stage. Symptom in
    // play: attacks connecting when the hitbox was nowhere near the opponent.
    //
    // fx_len_sq / fx_sq stay in 64-bit, so there is nothing to wrap.
    const fx r = fxi(24);
    const int64_t r2 = fx_sq(r);

    check(fx_len_sq(fxi(10), 0) <= r2, "10 units apart is inside a 24 radius");
    check(fx_len_sq(fxi(30), 0) > r2, "30 units apart is outside");

    // The exact distances that used to wrap negative and register as hits.
    check(fx_len_sq(fxi(182), 0) > r2, "182 units apart does NOT hit (was overflow)");
    check(fx_len_sq(fxi(200), 0) > r2, "200 units apart does NOT hit (was overflow)");
    check(fx_len_sq(fxi(500), 0) > r2, "500 units apart does NOT hit (was overflow)");
    check(fx_len_sq(0, fxi(400)) > r2, "400 units vertically does NOT hit");
    check(fx_len_sq(fxi(600), fxi(600)) > r2, "diagonal across the stage does NOT hit");

    // Monotonicity: squared distance must never decrease as separation grows. The
    // overflow broke exactly this, which is why far hits looked random.
    int64_t prev = -1;
    bool monotonic = true;
    for (int px = 0; px <= 1200; px += 20) {
        const int64_t d2 = fx_len_sq(fxi(px), 0);
        if (d2 < prev) monotonic = false;
        prev = d2;
    }
    check(monotonic, "squared distance grows monotonically out to 1200 units");
}

void testAttackReachIsBounded() {
    section("attacks only connect at their actual reach");

    // End-to-end guard on the same bug: sweep a defender away from a jabbing
    // attacker and confirm the hit stops landing once they are out of range.
    const auto& body = config::kFighter.body;
    const auto& jab = config::kScoutAttacks[config::ATK_JAB];

    // Furthest gap a jab can legitimately cover: reach + radius + half body.
    const int maxReachPx = fx_to_int(jab.reachX + jab.radius + body.halfWidth) + 2;

    int lastHitPx = -1;
    for (int gap = 10; gap <= 900; gap += 5) {
        GameState gs = makeMatch(2);
        gs.players[0].x = fxi(400);
        gs.players[0].facing = 1;
        gs.players[1].x = fxi(400) + fxi(gap);

        Sequence seq;
        seq.frames[0].push_back(mk(BtnAttack));
        for (int f = 1; f < 10; ++f) seq.frames[0].push_back(mk());
        run(gs, seq, 8);

        if (gs.players[1].damage > 0) lastHitPx = gap;
    }

    check(lastHitPx > 0, "jab connects at close range");
    check(lastHitPx <= maxReachPx, "jab never connects beyond its geometric reach");

    // Specifically: nothing lands at the distances the overflow used to hit.
    for (int gap : {200, 300, 500, 800}) {
        GameState gs = makeMatch(2);
        gs.players[0].x = fxi(400);
        gs.players[0].facing = 1;
        gs.players[1].x = fxi(400) + fxi(gap);

        Sequence seq;
        seq.frames[0].push_back(mk(BtnAttack));
        for (int f = 1; f < 10; ++f) seq.frames[0].push_back(mk());
        run(gs, seq, 8);

        checkEq(fx_to_int(gs.players[1].damage), 0, "no phantom hit at long range");
    }

    // And attacks must not hit BEHIND the attacker.
    GameState behind = makeMatch(2);
    behind.players[0].x = fxi(400);
    behind.players[0].facing = 1;         // facing right
    behind.players[1].x = fxi(360);       // standing to the left
    Sequence seq;
    seq.frames[0].push_back(mk(BtnAttack));
    for (int f = 1; f < 10; ++f) seq.frames[0].push_back(mk());
    run(behind, seq, 8);
    checkEq(fx_to_int(behind.players[1].damage), 0, "jab does not hit behind");
}

void testPerCharacterStats() {
    section("character stats are per-player, not global");

    using namespace config;

    // Roster sanity: every fighter must have an attack table, or attackOf would
    // dereference null.
    for (int c = 0; c < CHAR_COUNT; ++c) {
        check(kFighters[c].attacks != nullptr, "fighter has an attack table");
    }

    // The two characters must actually differ, otherwise the per-character
    // plumbing is untested by construction.
    const Fighter& scout = kFighters[CHAR_SCOUT];
    const Fighter& bruiser = kFighters[CHAR_BRUISER];
    check(bruiser.body.weight > scout.body.weight, "bruiser is heavier");
    check(bruiser.body.height > scout.body.height, "bruiser is taller");
    check(bruiser.ground.dashSpeed < scout.ground.dashSpeed, "bruiser dashes slower");
    check(bruiser.air.gravity > scout.air.gravity, "bruiser falls faster");
    check(bruiser.jump.jumpsquatFrames > scout.jump.jumpsquatFrames,
          "bruiser has a slower jumpsquat");
    check(bruiser.attacks != scout.attacks, "characters have distinct attack tables");

    // --- Gravity is read per-player ------------------------------------------
    // Two players, different characters, identical airborne start. After the same
    // number of frames the heavier character must have fallen further. If the sim
    // read a global fighter, these would be identical.
    GameState gs = makeMatch(2);
    for (int i = 0; i < 2; ++i) {
        Player& p = gs.players[i];
        p.charId = (i == 0) ? CHAR_SCOUT : CHAR_BRUISER;
        p.state = ActionState::Airborne;
        p.y = gs.stage.groundY - fxi(300);
        p.selfVelY = 0;
        p.x = fxi(400) + fxi(300) * i;
    }

    Sequence idle;
    for (int f = 0; f < 20; ++f) {
        idle.frames[0].push_back(mk());
        idle.frames[1].push_back(mk());
    }
    run(gs, idle, 20);

    check(gs.players[1].selfVelY > gs.players[0].selfVelY,
          "heavier character accelerates downward faster (per-player gravity)");

    // --- Jump height is read per-player --------------------------------------
    GameState jump = makeMatch(2);
    jump.players[0].charId = CHAR_SCOUT;
    jump.players[1].charId = CHAR_BRUISER;

    Sequence hold;
    for (int f = 0; f < 12; ++f) {
        hold.frames[0].push_back(mk(BtnJump));
        hold.frames[1].push_back(mk(BtnJump));
    }
    // Scout's jumpsquat is shorter, so sample after the longer of the two.
    run(jump, hold, bruiser.jump.jumpsquatFrames);
    check(jump.players[0].selfVelY != jump.players[1].selfVelY,
          "characters take off with different velocities");
}

void testPerCharacterAttacks() {
    section("attack tables are per-character");

    using namespace config;

    // Same AttackId, different character -> different frame data.
    check(kBruiserAttacks[ATK_SMASH_SIDE].startup >
              kScoutAttacks[ATK_SMASH_SIDE].startup,
          "bruiser side smash is slower to start");
    check(kBruiserAttacks[ATK_SMASH_SIDE].damage >
              kScoutAttacks[ATK_SMASH_SIDE].damage,
          "bruiser side smash hits harder");
    check(kBruiserAttacks[ATK_JAB].startup > kScoutAttacks[ATK_JAB].startup,
          "even the bruiser jab is more committal");

    // Both tables must be internally coherent, not just different.
    for (const auto* table : {kScoutAttacks, kBruiserAttacks}) {
        for (int i = 1; i < ATK_COUNT; ++i) {
            check(table[i].active > 0, "attack has active frames");
            check(table[i].damage > 0, "attack deals damage");
            check(table[i].radius > 0, "attack has a radius");
            check(table[i].total >= table[i].startup + table[i].active,
                  "total contains the active window");
        }
        // Only smashes charge, in every table.
        for (int i = 1; i < ATK_COUNT; ++i) {
            const bool isSmash = (i == ATK_SMASH_SIDE || i == ATK_SMASH_UP ||
                                  i == ATK_SMASH_DOWN);
            checkEq(table[i].chargeable ? 1 : 0, isSmash ? 1 : 0,
                    "only smashes are chargeable");
        }
    }

    // The attacker's OWN table decides what comes out. A bruiser jab must deal
    // bruiser damage, not scout damage.
    auto jabDamage = [](uint8_t attackerChar) {
        GameState g = makeMatch(2);
        g.players[0].charId = attackerChar;
        g.players[1].charId = CHAR_SCOUT;
        g.players[0].x = fxi(500);
        g.players[0].facing = 1;
        g.players[1].x = fxi(524);

        Input pv[tf::kMaxPlayers] = {};
        auto t = [&](const Input& a) {
            Input cur[tf::kMaxPlayers] = {};
            cur[0] = a;
            step(g, cur, pv);
            std::memcpy(pv, cur, sizeof(cur));
        };
        t(mk(BtnAttack));
        for (int f = 0; f < 30; ++f) t(mk());
        return fx_to_int(g.players[1].damage);
    };

    const int scoutJab = jabDamage(CHAR_SCOUT);
    const int bruiserJab = jabDamage(CHAR_BRUISER);
    check(scoutJab > 0, "scout jab connects");
    check(bruiserJab > 0, "bruiser jab connects");
    check(bruiserJab > scoutJab, "bruiser jab deals more damage than scout jab");
}

void testDefenderBodyDecidesHurtbox() {
    section("defender's own body sizes their hurtbox and resists knockback");

    using namespace config;

    // REGRESSION: resolveAttack previously used a single global fighter for the
    // DEFENDER's hurtbox and weight. With per-character bodies that would size
    // every hurtbox by whoever was swinging, and ignore the defender's weight in
    // the knockback formula.
    //
    // Same attacker, same attack, same distance -- only the victim's character
    // differs. The heavier victim must take LESS knockback.
    auto knockbackOn = [](uint8_t victimChar) {
        GameState g = makeMatch(2);
        g.players[0].charId = CHAR_SCOUT;
        g.players[1].charId = victimChar;
        g.players[0].x = fxi(500);
        g.players[0].facing = 1;
        g.players[1].x = fxi(524);
        g.players[1].damage = fxi(60);

        Input pv[tf::kMaxPlayers] = {};
        auto t = [&](const Input& a) {
            Input cur[tf::kMaxPlayers] = {};
            cur[0] = a;
            step(g, cur, pv);
            std::memcpy(pv, cur, sizeof(cur));
        };
        t(mk(BtnAttack));
        for (int f = 0; f < 6; ++f) t(mk());
        return fx_abs(g.players[1].kbVelX) + fx_abs(g.players[1].kbVelY);
    };

    const fx vsScout = knockbackOn(CHAR_SCOUT);
    const fx vsBruiser = knockbackOn(CHAR_BRUISER);
    check(vsScout > 0, "scout victim was launched");
    check(vsBruiser > 0, "bruiser victim was launched");
    check(vsBruiser < vsScout,
          "heavier defender takes LESS knockback (defender weight is used)");

    // A taller defender is a bigger target: a hitbox that misses a Scout at a
    // given height should still catch a Bruiser.
    check(kFighters[CHAR_BRUISER].body.height > kFighters[CHAR_SCOUT].body.height,
          "bruiser hurtbox is taller");
    check(kFighters[CHAR_BRUISER].body.halfWidth >
              kFighters[CHAR_SCOUT].body.halfWidth,
          "bruiser hurtbox is wider");
}

void testInvalidCharIdIsSafe() {
    section("out-of-range charId is handled safely");

    // charId arrives from the network. An out-of-range value must produce a
    // DEFINED result rather than reading past the roster -- a wrong character is
    // recoverable, undefined behavior is not.
    GameState gs = makeMatch(1);
    gs.players[0].charId = 250;      // far outside the roster
    gs.players[0].state = ActionState::Airborne;
    gs.players[0].y = gs.stage.groundY - fxi(100);

    Sequence seq;
    for (int f = 0; f < 30; ++f) seq.frames[0].push_back(mk(BtnAttack, 99, 0));
    run(gs, seq, 30);   // must not crash

    check(gs.players[0].active, "player with invalid charId still simulates");
    check(gs.tick == 30, "simulation advanced normally");
}

// --- Knockdown / tech -------------------------------------------------------

// Launch a player into the ground at a chosen knockback velocity and run until a
// resolution state is reached. `techFrame` presses shield that many frames in (-1
// for never), which is how tech timing is exercised.
struct ImpactResult {
    ActionState state = ActionState::Idle;
    int framesToResolve = -1;
    bool bounced = false;
};

ImpactResult dropIntoGround(fx kbx, fx kby, int techFrame, uint8_t charId,
                            int maxFrames = 240, fx dropHeight = fxi(40)) {
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.charId = charId;
    p.state = ActionState::Hitstun;
    p.hitstunFrames = 40;
    p.x = fxi(600);
    p.y = gs.stage.groundY - dropHeight;
    p.kbVelX = kbx;
    p.kbVelY = kby;

    Input prev[tf::kMaxPlayers] = {};
    ImpactResult r;
    for (int f = 0; f < maxFrames; ++f) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk((f == techFrame) ? BtnShield : 0);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));

        if (gs.players[0].state == ActionState::Bounce) r.bounced = true;
        const ActionState st = gs.players[0].state;
        if (st == ActionState::DownWait || st == ActionState::Tech ||
            st == ActionState::Landing) {
            r.state = st;
            r.framesToResolve = f;
            return r;
        }
    }
    r.state = gs.players[0].state;
    return r;
}

void testImpactSpeedDecidesOutcome() {
    section("hitstun landing branches on impact speed");

    using namespace config;
    const Knockdown& K = kFighters[CHAR_SCOUT].knockdown;

    // The central mechanic, mirroring ftCo_Damage_Coll: the outcome is decided by
    // the knockback SURVIVING at ground contact, not by the hit that caused it.

    // Hard slam -> bounce, then knocked down.
    const ImpactResult hard = dropIntoGround(0, fxi(8), -1, CHAR_SCOUT);
    check(hard.bounced, "hard impact bounces");
    check(hard.state == ActionState::DownWait, "hard impact ends in knockdown");

    // Moderate impact -> straight to knockdown, no bounce.
    const fx moderate = (K.bounceThreshold + K.hardLandThreshold) / 2;
    const ImpactResult mid = dropIntoGround(0, moderate, -1, CHAR_SCOUT);
    check(!mid.bounced, "moderate impact does NOT bounce");
    check(mid.state == ActionState::DownWait, "moderate impact knocks down");

    // Soft impact -> ordinary landing. The knockback decayed enough that this is
    // just a landing, which is why drifting out a launch is worth doing.
    const ImpactResult soft = dropIntoGround(0, fx_ratio(3, 10), -1, CHAR_SCOUT);
    check(!soft.bounced, "soft impact does not bounce");
    check(soft.state == ActionState::Landing, "soft impact is a normal landing");

    // Monotonic: harder impacts must never produce a gentler outcome.
    auto severity = [](ActionState st) {
        return st == ActionState::DownWait ? 2 : (st == ActionState::Landing ? 1 : 0);
    };
    check(severity(hard.state) >= severity(mid.state), "hard >= moderate severity");
    check(severity(mid.state) >= severity(soft.state), "moderate >= soft severity");
}

void testTechBeatsKnockdown() {
    section("teching avoids knockdown");

    using namespace config;

    // A tech at the right moment converts a hard slam into instant recovery. This
    // is the counterplay that makes combos escapable -- without it, landing in
    // hitstun has no interaction at all.
    const ImpactResult teched = dropIntoGround(0, fxi(8), 0, CHAR_SCOUT);
    check(teched.state == ActionState::Tech, "well-timed tech avoids knockdown");

    const ImpactResult notTeched = dropIntoGround(0, fxi(8), -1, CHAR_SCOUT);
    check(notTeched.state == ActionState::DownWait, "no tech -> knockdown");

    // Recovering from a tech must be faster than from a knockdown, or teching
    // would be pointless.
    GameState techGs = makeMatch(1);
    {
        Player& p = techGs.players[0];
        p.state = ActionState::Tech;
        p.y = techGs.stage.groundY;
    }
    int techRecovery = 0;
    {
        Input prev[tf::kMaxPlayers] = {};
        for (int f = 0; f < 200; ++f) {
            Input cur[tf::kMaxPlayers] = {};
            step(techGs, cur, prev);
            ++techRecovery;
            if (techGs.players[0].state == ActionState::Idle) break;
        }
    }
    const Knockdown& K = kFighters[CHAR_SCOUT].knockdown;
    check(techRecovery <= K.techFrames + 1, "tech recovery matches config");
    check(techRecovery < K.downWaitMaxFrames,
          "teching recovers faster than lying down does");
}

void testTechLockoutPreventsMashing() {
    section("mistimed tech locks you out (mashing is not optimal)");

    using namespace config;

    // A tech attempt that expires before reaching the ground must LOCK OUT further
    // attempts. Without this, holding shield through every launch would be free.
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.state = ActionState::Hitstun;
    p.hitstunFrames = 200;          // long hitstun: stay airborne a while
    p.x = fxi(600);
    p.y = gs.stage.groundY - fxi(600);   // far above the ground
    p.kbVelX = 0;
    p.kbVelY = 0;

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](uint16_t b) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(b);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    tick(BtnShield);   // tech attempt, nowhere near the ground
    check(gs.players[0].techWindow > 0, "tech window opened");

    const Knockdown& K = kFighters[CHAR_SCOUT].knockdown;
    for (int f = 0; f < K.techWindow + 2; ++f) tick(0);
    checkEq(gs.players[0].techWindow, 0, "window expired");
    check(gs.players[0].techLockout > 0, "expiry causes a LOCKOUT");

    // During lockout a new attempt must not open a window.
    tick(BtnShield);
    checkEq(gs.players[0].techWindow, 0, "no tech window while locked out");

    // Lockout must eventually clear, or one bad attempt would be permanent.
    for (int f = 0; f < K.techLockoutFrames + 4; ++f) tick(0);
    checkEq(gs.players[0].techLockout, 0, "lockout expires");
    tick(BtnShield);
    check(gs.players[0].techWindow > 0, "tech is available again after lockout");
}

void testGetUpOptions() {
    section("get-up options from knockdown");

    using namespace config;
    const Knockdown& K = kFighters[CHAR_SCOUT].knockdown;

    // Put a player into DownWait, feed an option, and report where they end up.
    auto fromKnockdown = [&](uint16_t buttons, int8_t stickX, int settleFrames) {
        GameState gs = makeMatch(1);
        Player& p = gs.players[0];
        p.state = ActionState::DownWait;
        p.y = gs.stage.groundY;
        p.x = fxi(600);

        Input prev[tf::kMaxPlayers] = {};
        ActionState reached = ActionState::DownWait;
        for (int f = 0; f < settleFrames; ++f) {
            Input cur[tf::kMaxPlayers] = {};
            cur[0] = mk(buttons, stickX);
            step(gs, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
            const ActionState st = gs.players[0].state;
            if (st != ActionState::DownWait) { reached = st; break; }
        }
        return std::pair<ActionState, fx>{reached, gs.players[0].x};
    };

    const auto rolled = fromKnockdown(0, 99, 60);
    check(rolled.first == ActionState::GetUpRoll, "stick sideways -> roll");

    const auto attacked = fromKnockdown(BtnAttack, 0, 60);
    check(attacked.first == ActionState::GetUpAttack, "attack -> get-up attack");

    const auto stood = fromKnockdown(BtnJump, 0, 60);
    check(stood.first == ActionState::GetUp, "jump -> neutral get-up");

    const auto shielded = fromKnockdown(BtnShield, 0, 60);
    check(shielded.first == ActionState::GetUp, "shield -> neutral get-up");

    // Options must be LOCKED OUT briefly, so a knockdown always costs something.
    GameState early = makeMatch(1);
    early.players[0].state = ActionState::DownWait;
    early.players[0].y = early.stage.groundY;
    {
        Input prev[tf::kMaxPlayers] = {};
        for (int f = 0; f < K.downWaitMinFrames - 1; ++f) {
            Input cur[tf::kMaxPlayers] = {};
            cur[0] = mk(BtnJump);
            step(early, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
        }
        check(early.players[0].state == ActionState::DownWait,
              "cannot get up before the minimum delay");
    }

    // A forced get-up caps the maximum, so nobody can lie there indefinitely.
    GameState idle = makeMatch(1);
    idle.players[0].state = ActionState::DownWait;
    idle.players[0].y = idle.stage.groundY;
    {
        Input prev[tf::kMaxPlayers] = {};
        for (int f = 0; f < K.downWaitMaxFrames + 4; ++f) {
            Input cur[tf::kMaxPlayers] = {};
            step(idle, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
        }
        check(idle.players[0].state != ActionState::DownWait,
              "forced get-up after the maximum");
    }

    // Rolling must actually move you -- that's its whole purpose.
    check(rolled.second != fxi(600), "roll repositions the player");
}

void testGetUpInputIsBuffered() {
    section("get-up input pressed during lockout is buffered");

    using namespace config;
    const Knockdown& K = kFighters[CHAR_SCOUT].knockdown;

    // REGRESSION: get-up used pressed() (a rising edge), but options are locked
    // for the first few frames. A player mashing jump on knockdown had the press
    // consumed during lockout and silently eaten -- they had to release and
    // re-press. Buffering fixes it.
    GameState gs = makeMatch(1);
    gs.players[0].state = ActionState::DownWait;
    gs.players[0].y = gs.stage.groundY;

    Input prev[tf::kMaxPlayers] = {};
    // Press jump ONCE on the very first frame -- inside the lockout -- then hold
    // nothing. The press must still be honoured when options unlock.
    int actedOn = -1;
    for (int f = 0; f < 80; ++f) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(f == 0 ? BtnJump : 0);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
        if (gs.players[0].state != ActionState::DownWait) { actedOn = f; break; }
    }
    check(actedOn >= 0, "buffered press eventually acted on");
    check(actedOn < K.downWaitMaxFrames - 1,
          "acted on the buffered press, not the forced get-up");
    check(gs.players[0].state == ActionState::GetUp, "buffered jump gave a get-up");
}

void testKnockdownIsPerCharacter() {
    section("knockdown values are per-character");

    using namespace config;
    const Knockdown& scout = kFighters[CHAR_SCOUT].knockdown;
    const Knockdown& bruiser = kFighters[CHAR_BRUISER].knockdown;

    // The heavier character must be harder to knock down and slower to recover --
    // otherwise the per-character plumbing is untested here.
    check(bruiser.bounceThreshold > scout.bounceThreshold,
          "bruiser needs a harder slam to bounce");
    check(bruiser.techWindow < scout.techWindow,
          "bruiser has a tighter tech window");
    check(bruiser.getUpFrames > scout.getUpFrames, "bruiser gets up slower");

    // An impact between the two thresholds must bounce the Scout but not the
    // Bruiser -- the same slam, different character, different outcome.
    //
    // Dropped from a SHORT height on purpose: knockback decays during the fall, so
    // a long drop would test the decay rather than the threshold. (That decay is
    // real and intended -- see testImpactSpeedDecidesOutcome -- it just makes this
    // particular comparison meaningless if the fall is long.)
    const fx between = (scout.bounceThreshold + bruiser.bounceThreshold) / 2;
    const fx shortDrop = fxi(4);
    const ImpactResult s = dropIntoGround(0, between, -1, CHAR_SCOUT, 240, shortDrop);
    const ImpactResult b = dropIntoGround(0, between, -1, CHAR_BRUISER, 240, shortDrop);
    check(s.bounced, "scout bounces at this impact speed");
    check(!b.bounced, "bruiser does NOT bounce at the same speed");

    // And the decay itself is worth asserting: the SAME launch speed dropped from
    // high up must produce a gentler outcome than dropped from just above the
    // ground, because the knockback has more time to bleed off.
    const ImpactResult near = dropIntoGround(0, scout.bounceThreshold + fx_ratio(3, 10),
                                             -1, CHAR_SCOUT, 240, fxi(4));
    const ImpactResult far = dropIntoGround(0, scout.bounceThreshold + fx_ratio(3, 10),
                                            -1, CHAR_SCOUT, 240, fxi(300));
    check(near.bounced, "impact just above the ground bounces");
    check(!far.bounced, "same launch from high up decays below the bounce threshold");
}

void testBounceCannotLoop() {
    section("bounce cannot repeat indefinitely");

    // A very hard slam must resolve, not pinball forever. bounceCount caps it.
    const ImpactResult violent =
        dropIntoGround(fxi(6), fxi(20), -1, config::CHAR_SCOUT);
    check(violent.state == ActionState::DownWait || violent.state == ActionState::Landing,
          "extreme impact still resolves to a settled state");
    check(violent.framesToResolve >= 0, "resolution happened within the frame budget");
}

// --- Ledges -----------------------------------------------------------------

// Place a player just off the right ledge and run until they either catch it or
// fall past. Returns the state reached.
struct LedgeTrial {
    ActionState state = ActionState::Airborne;
    bool caught = false;
    int  frame = -1;
    fx   x = 0, y = 0;
};

LedgeTrial approachLedge(uint8_t charId, fx offsetX, fx offsetY, fx velY,
                         ActionState startState = ActionState::Airborne,
                         uint8_t cooldown = 0, int frames = 40,
                         int8_t facing = -1) {
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.charId = charId;
    p.state = startState;
    p.x = gs.stage.platformRight + offsetX;
    p.y = gs.stage.groundY + offsetY;
    p.selfVelY = velY;
    p.ledgeCooldown = cooldown;
    // Approaching the RIGHT ledge, so facing the stage means facing LEFT (-1).
    // Melee requires you to face a ledge to catch it, which is what makes recovery
    // deliberate rather than a proximity check.
    p.facing = facing;
    if (startState == ActionState::Hitstun) p.hitstunFrames = 30;

    Input prev[tf::kMaxPlayers] = {};
    LedgeTrial r;
    for (int f = 0; f < frames; ++f) {
        Input cur[tf::kMaxPlayers] = {};
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
        if (gs.players[0].state == ActionState::LedgeHang) {
            r.caught = true;
            r.frame = f;
            break;
        }
    }
    r.state = gs.players[0].state;
    r.x = gs.players[0].x;
    r.y = gs.players[0].y;
    return r;
}

void testLedgeGrab() {
    section("ledge grab detection");

    using namespace config;
    const Ledge& L = kFighters[CHAR_SCOUT].ledge;

    // In range, descending, on the outward side -> caught.
    const LedgeTrial ok = approachLedge(CHAR_SCOUT, fxi(10), fxi(20), fx_ratio(10, 10));
    check(ok.caught, "descending near the ledge catches it");

    // Too far out horizontally -> no catch.
    const LedgeTrial farOut =
        approachLedge(CHAR_SCOUT, L.grabReachX + fxi(40), fxi(20), fx_ratio(10, 10));
    check(!farOut.caught, "too far horizontally does not catch");

    // Too far below -> no catch.
    const LedgeTrial low =
        approachLedge(CHAR_SCOUT, fxi(10), L.grabReachDown + fxi(60), fx_ratio(10, 10));
    check(!low.caught, "too far below does not catch");

    // RISING past the ledge must not catch it -- otherwise jumping up past a stage
    // would snap you on, making recovery magnetic instead of deliberate.
    //
    // Asserted only for the frames the player is actually ASCENDING. Once gravity
    // turns them around they are descending beside the ledge and SHOULD catch it --
    // jumping up, peaking, and falling onto the ledge is legitimate recovery, so
    // running this to completion would be testing the wrong invariant.
    {
        GameState gs = makeMatch(1);
        Player& pl = gs.players[0];
        pl.state = ActionState::Airborne;
        pl.x = gs.stage.platformRight + fxi(10);
        pl.y = gs.stage.groundY + fxi(30);
        pl.selfVelY = fx_ratio(-40, 10);   // rising
        pl.facing = -1;

        Input prev[tf::kMaxPlayers] = {};
        bool caughtWhileRising = false;
        int risingFrames = 0;
        for (int f = 0; f < 60; ++f) {
            Input cur[tf::kMaxPlayers] = {};
            step(gs, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
            // Sampled AFTER the step: gravity is applied inside step(), so at the
            // apex frame a player who began rising is already descending by the
            // time the ledge check runs -- and catching then is legitimate.
            if (gs.players[0].totalVelY() >= 0) break;
            ++risingFrames;
            if (gs.players[0].state == ActionState::LedgeHang) {
                caughtWhileRising = true;
                break;
            }
        }
        check(risingFrames > 0, "player spent frames ascending past the ledge");
        check(!caughtWhileRising, "rising past the ledge does NOT catch");
    }

    // The complement: once descending, the same position DOES catch. Recovery has
    // to actually work, not just be blocked.
    const LedgeTrial descending =
        approachLedge(CHAR_SCOUT, fxi(10), fxi(30), fx_ratio(10, 10));
    check(descending.caught, "descending at the same spot DOES catch");

    // Inward side (over the stage) must not catch: you would grab through it.
    const LedgeTrial inward =
        approachLedge(CHAR_SCOUT, fxi(-60), fxi(20), fx_ratio(10, 10));
    check(!inward.caught, "cannot catch from the stage side");

    // Cooldown blocks the grab entirely -- the anti-stalling rule.
    const LedgeTrial cooling =
        approachLedge(CHAR_SCOUT, fxi(10), fxi(20), fx_ratio(10, 10),
                      ActionState::Airborne, 20);
    check(!cooling.caught, "cooldown prevents regrab");
}

void testLedgeRequiresFacing() {
    section("must be FACING the ledge to catch it");

    using namespace config;

    // Melee requires facing the ledge. This is what makes recovery deliberate
    // rather than a proximity check: drifting backwards past the edge does NOT save
    // you, you have to turn around first, and that costs time you may not have.
    const LedgeTrial facingStage =
        approachLedge(CHAR_SCOUT, fxi(10), fxi(20), fx_ratio(10, 10),
                      ActionState::Airborne, 0, 40, /*facing*/-1);
    check(facingStage.caught, "facing the stage catches the right ledge");

    const LedgeTrial facingAway =
        approachLedge(CHAR_SCOUT, fxi(10), fxi(20), fx_ratio(10, 10),
                      ActionState::Airborne, 0, 40, /*facing*/+1);
    check(!facingAway.caught, "facing AWAY from the stage does NOT catch");

    // Same rule mirrored on the left ledge: there, facing the stage is facing RIGHT.
    auto approachLeft = [](int8_t facing) {
        GameState gs = makeMatch(1);
        Player& p = gs.players[0];
        p.state = ActionState::Airborne;
        p.x = gs.stage.platformLeft - fxi(10);
        p.y = gs.stage.groundY + fxi(20);
        p.selfVelY = fx_ratio(10, 10);
        p.facing = facing;

        Input prev[tf::kMaxPlayers] = {};
        for (int f = 0; f < 40; ++f) {
            Input cur[tf::kMaxPlayers] = {};
            step(gs, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
            if (gs.players[0].state == ActionState::LedgeHang) return true;
        }
        return false;
    };
    check(approachLeft(+1), "facing right catches the LEFT ledge");
    check(!approachLeft(-1), "facing left does not catch the left ledge");

    // The rule is a per-character switch, so it can be relaxed for a character
    // whose identity is forgiving recovery.
    check(kFighters[CHAR_SCOUT].ledge.requireFacing,
          "facing requirement is enabled by default");
}

void testHitstunCannotAutoGrabLedge() {
    section("hitstun does NOT auto-grab the ledge");

    using namespace config;

    // REGRESSION: canGrabLedge originally included Hitstun and Bounce, which meant
    // being launched off-stage auto-snapped you to the ledge with no input -- a free
    // save that gutted the KO system, since almost nothing near the ledge could
    // kill. Recovery must be deliberate: act out of hitstun first, then grab.
    const LedgeTrial launched =
        approachLedge(CHAR_SCOUT, fxi(10), fxi(20), fx_ratio(10, 10),
                      ActionState::Hitstun);
    check(!launched.caught, "a player in hitstun cannot catch the ledge");

    // End-to-end: a launched victim near the ledge must actually die rather than
    // being rescued by an automatic grab.
    GameState gs = makeMatch(2);
    gs.players[0].x = gs.stage.platformRight - fxi(30);
    gs.players[0].facing = 1;
    gs.players[0].damage = fxi(85);
    gs.players[1].x = gs.stage.platformRight - fxi(10);
    gs.players[1].damage = fxi(300);
    const int16_t startStocks = gs.players[1].stocks;

    Input prev[tf::kMaxPlayers] = {};
    for (int f = 0; f < 400; ++f) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(f == 0 ? BtnAttack : 0);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    }
    check(gs.players[1].stocks < startStocks,
          "victim launched off-stage still dies (not saved by the ledge)");
}

void testLedgeHangBehavior() {
    section("hanging on the ledge");

    using namespace config;
    const Ledge& L = kFighters[CHAR_SCOUT].ledge;

    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.state = ActionState::Airborne;
    p.x = gs.stage.platformRight + fxi(10);
    p.y = gs.stage.groundY + fxi(20);
    p.selfVelY = fx_ratio(10, 10);
    p.facing = -1;   // facing the stage: required to catch the right ledge

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](const Input& i0) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = i0;
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    for (int f = 0; f < 20 && gs.players[0].state != ActionState::LedgeHang; ++f) {
        tick(mk());
    }
    check(gs.players[0].state == ActionState::LedgeHang, "caught the ledge");
    checkEq(gs.players[0].ledgeSide, 1, "caught the RIGHT ledge");
    checkEq(gs.players[0].facing, -1, "faces the stage while hanging");
    // (Entry already required facing the stage, so this confirms the
    //  hang does not flip it back out over the void.)
    check(gs.players[0].invulnFrames > 0, "brief invulnerability on catching");
    checkEq(gs.players[0].airJumps, 0, "air jump is restored by the ledge");

    // Position is pinned and velocity zeroed: hanging is a hard stop.
    const fx hangX = gs.players[0].x;
    const fx hangY = gs.players[0].y;
    for (int f = 0; f < 30; ++f) tick(mk());
    checkEq(gs.players[0].x, hangX, "x is pinned while hanging");
    checkEq(gs.players[0].y, hangY, "y is pinned while hanging (no gravity)");
    checkEq(gs.players[0].selfVelY, 0, "no vertical velocity while hanging");
    check(gs.players[0].state == ActionState::LedgeHang, "still hanging");

    // The hang countdown must be ticking toward a forced release.
    check(gs.players[0].ledgeHangFrames < L.hangFrames, "hang timer counts down");
}

void testLedgeOptions() {
    section("ledge get-up options");

    using namespace config;

    // Drop onto the ledge, then feed an input and report where it leads.
    auto fromLedge = [](uint16_t buttons, int8_t stickX, int8_t stickY,
                        fx damage, int settle) {
        GameState gs = makeMatch(1);
        Player& p = gs.players[0];
        p.state = ActionState::Airborne;
        p.x = gs.stage.platformRight + fxi(10);
        p.y = gs.stage.groundY + fxi(20);
        p.selfVelY = fx_ratio(10, 10);
        p.facing = -1;   // facing the stage: required to catch the right ledge

        Input prev[tf::kMaxPlayers] = {};
        auto tick = [&](const Input& i0) {
            Input cur[tf::kMaxPlayers] = {};
            cur[0] = i0;
            step(gs, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
        };
        for (int f = 0; f < 20 && gs.players[0].state != ActionState::LedgeHang; ++f) {
            tick(mk());
        }
        gs.players[0].damage = damage;

        ActionState reached = ActionState::LedgeHang;
        for (int f = 0; f < settle; ++f) {
            tick(mk(buttons, stickX, stickY));
            if (gs.players[0].state != ActionState::LedgeHang) {
                reached = gs.players[0].state;
                break;
            }
        }
        return std::pair<ActionState, GameState>{reached, gs};
    };

    // Climb: hold toward the stage. Right ledge -> stage is to the LEFT.
    const auto climbed = fromLedge(0, -99, 0, 0, 20);
    check(climbed.first == ActionState::LedgeClimb, "toward stage -> climb");

    // Roll: shield.
    const auto rolled = fromLedge(BtnShield, 0, 0, 0, 20);
    check(rolled.first == ActionState::LedgeRoll, "shield -> ledge roll");

    // Attack: reaches onto the stage.
    const auto attacked = fromLedge(BtnAttack, 0, 0, 0, 20);
    check(attacked.first == ActionState::LedgeAttack, "attack -> ledge attack");

    // Jump.
    const auto jumped = fromLedge(BtnJump, 0, 0, 0, 20);
    check(jumped.first == ActionState::LedgeJump, "jump -> ledge jump");

    // Drop off: hold down, or away from the stage.
    const auto dropped = fromLedge(0, 0, 99, 0, 20);
    check(dropped.first == ActionState::Airborne, "down -> drop off the ledge");

    const auto awayDrop = fromLedge(0, 99, 0, 0, 20);
    check(awayDrop.first == ActionState::Airborne, "away from stage -> drop off");

    // Every exit must set the regrab cooldown. That single guarantee is what
    // prevents infinite invulnerable ledge camping.
    check(climbed.second.players[0].ledgeCooldown > 0, "climb sets cooldown");
    check(rolled.second.players[0].ledgeCooldown > 0, "roll sets cooldown");
    check(attacked.second.players[0].ledgeCooldown > 0, "attack sets cooldown");
    check(jumped.second.players[0].ledgeCooldown > 0, "jump sets cooldown");
    check(dropped.second.players[0].ledgeCooldown > 0, "drop sets cooldown");
    check(climbed.second.players[0].ledgeSide < 0, "ledge released on exit");
}

void testLedgeSlowQuickByDamage() {
    section("ledge options are slower at high damage");

    using namespace config;
    const Ledge& L = kFighters[CHAR_SCOUT].ledge;

    // Slow vs quick is one damage comparison, exactly as the decomp does it. Being
    // badly damaged makes recovery itself more punishable -- pressure compounds
    // precisely when you can least afford it.
    check(L.climbSlowFrames > L.climbQuickFrames, "slow climb is slower");
    check(L.rollSlowFrames > L.rollQuickFrames, "slow roll is slower");
    check(L.attackSlowFrames > L.attackQuickFrames, "slow ledge attack is slower");

    // Measure the actual latched frame count at low vs high damage.
    auto climbFrames = [&](fx damage) {
        GameState gs = makeMatch(1);
        Player& p = gs.players[0];
        p.state = ActionState::Airborne;
        p.x = gs.stage.platformRight + fxi(10);
        p.y = gs.stage.groundY + fxi(20);
        p.selfVelY = fx_ratio(10, 10);
        p.facing = -1;   // facing the stage: required to catch the right ledge

        Input prev[tf::kMaxPlayers] = {};
        auto tick = [&](const Input& i0) {
            Input cur[tf::kMaxPlayers] = {};
            cur[0] = i0;
            step(gs, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
        };
        for (int f = 0; f < 20 && gs.players[0].state != ActionState::LedgeHang; ++f) {
            tick(mk());
        }
        gs.players[0].damage = damage;
        for (int f = 0; f < 10 && gs.players[0].state == ActionState::LedgeHang; ++f) {
            tick(mk(0, -99, 0));
        }
        return static_cast<int>(gs.players[0].ledgeActionFrames);
    };

    const int quick = climbFrames(fxi(0));
    const int slow = climbFrames(L.slowThreshold + fxi(20));
    checkEq(quick, L.climbQuickFrames, "low damage latches the quick climb");
    checkEq(slow, L.climbSlowFrames, "high damage latches the slow climb");
    check(slow > quick, "high damage climbs slower");
}

void testLedgeClimbPlacesOnStage() {
    section("climbing ends on the stage");

    using namespace config;

    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.state = ActionState::Airborne;
    p.x = gs.stage.platformRight + fxi(10);
    p.y = gs.stage.groundY + fxi(20);
    p.selfVelY = fx_ratio(10, 10);
    p.facing = -1;   // facing the stage: required to catch the right ledge

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](const Input& i0) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = i0;
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };
    for (int f = 0; f < 20 && gs.players[0].state != ActionState::LedgeHang; ++f) {
        tick(mk());
    }
    check(gs.players[0].state == ActionState::LedgeHang, "hanging");

    for (int f = 0; f < 120; ++f) {
        tick(mk(0, -99, 0));
        if (gs.players[0].state == ActionState::Idle) break;
    }
    check(gs.players[0].state == ActionState::Idle, "climb completes to Idle");
    checkEq(gs.players[0].y, gs.stage.groundY, "ends standing on the stage floor");
    check(gs.players[0].x < gs.stage.platformRight, "ends INSIDE the stage edge");
    check(gs.players[0].x > gs.stage.platformLeft, "ends within the platform");
}

void testHitOffLedgeSetsCooldown() {
    section("being hit off the ledge sets the regrab cooldown");

    using namespace config;

    // Without this you could be knocked off and instantly regrab with fresh
    // invulnerability -- the decomp sets the cooldown in ftCo_Damage for exactly
    // this reason.
    GameState gs = makeMatch(2);
    Player& hanger = gs.players[1];
    hanger.state = ActionState::LedgeHang;
    hanger.ledgeSide = 1;
    hanger.ledgeHangFrames = 200;
    hanger.x = gs.stage.platformRight + fxi(14);
    hanger.y = gs.stage.groundY + fxi(36);
    hanger.invulnFrames = 0;          // vulnerable, so the hit lands

    gs.players[0].x = hanger.x - fxi(24);
    gs.players[0].facing = 1;
    gs.players[0].y = gs.stage.groundY;

    Input prev[tf::kMaxPlayers] = {};
    for (int f = 0; f < 10; ++f) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(f == 0 ? BtnAttack : 0);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
        if (gs.players[1].state == ActionState::Hitstun) break;
    }

    check(gs.players[1].state == ActionState::Hitstun, "hanger was hit");
    checkEq(gs.players[1].ledgeSide, -1, "ledge released when hit");
    check(gs.players[1].ledgeCooldown > 0, "hit off the ledge sets the cooldown");
}

void testLedgeIsPerCharacter() {
    section("ledge values are per-character");

    using namespace config;
    const Ledge& scout = kFighters[CHAR_SCOUT].ledge;
    const Ledge& bruiser = kFighters[CHAR_BRUISER].ledge;

    check(bruiser.grabReachX > scout.grabReachX, "bruiser has longer ledge reach");
    check(bruiser.climbQuickFrames > scout.climbQuickFrames,
          "bruiser climbs slower");
    check(bruiser.rollQuickFrames > scout.rollQuickFrames, "bruiser rolls slower");

    // Both characters must actually be able to catch a ledge.
    const LedgeTrial s = approachLedge(CHAR_SCOUT, fxi(10), fxi(20), fx_ratio(10, 10));
    const LedgeTrial b = approachLedge(CHAR_BRUISER, fxi(10), fxi(20), fx_ratio(10, 10));
    check(s.caught, "scout catches the ledge");
    check(b.caught, "bruiser catches the ledge");
}

void testLedgeCannotStallForever() {
    section("ledge hanging times out (anti-stalling)");

    using namespace config;
    const Ledge& L = kFighters[CHAR_SCOUT].ledge;

    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.state = ActionState::LedgeHang;
    p.ledgeSide = 1;
    p.ledgeHangFrames = 8;      // nearly expired, to keep the test short
    p.x = gs.stage.platformRight + fxi(14);
    p.y = gs.stage.groundY + fxi(36);

    Input prev[tf::kMaxPlayers] = {};
    for (int f = 0; f < 20; ++f) {
        Input cur[tf::kMaxPlayers] = {};
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
        if (gs.players[0].state != ActionState::LedgeHang) break;
    }
    check(gs.players[0].state != ActionState::LedgeHang, "forced off after timeout");
    checkEq(gs.players[0].ledgeSide, -1, "ledge released on timeout");
    check(gs.players[0].ledgeCooldown > 0, "timeout also sets the cooldown");

    // And the configured hang time must be a real, finite limit.
    check(L.hangFrames > 0 && L.hangFrames < 3600, "hang time is finite");
}

void testWalkOffEdge() {
    section("walking off the edge falls (regression)");

    // REGRESSION: nothing re-checked whether the floor was still under a grounded
    // player, so you could stroll off the platform and keep walking on empty air.
    // One missing check produced two symptoms -- the second was that jumping from
    // out there dropped you straight THROUGH the stage, because on the way down the
    // landing test correctly found no platform beneath you.
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.x = gs.stage.platformRight - fxi(40);
    p.y = gs.stage.groundY;
    p.facing = 1;

    Sequence seq;
    for (int f = 0; f < 60; ++f) seq.frames[0].push_back(mk(0, 99));
    run(gs, seq, 60);

    check(gs.players[0].x > gs.stage.platformRight,
          "player walked past the platform edge");
    check(gs.players[0].state != ActionState::Idle &&
          gs.players[0].state != ActionState::Walk &&
          gs.players[0].state != ActionState::Dash,
          "player is no longer in a grounded state");
    check(gs.players[0].y > gs.stage.groundY,
          "player fell BELOW the stage floor (not standing on air)");
}

void testJumpNearEdgeLandsNormally() {
    section("jumping near the edge lands on the stage");

    // The other half of the same bug: a jump from just inside the edge must land
    // normally, not fall through.
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.x = gs.stage.platformRight - fxi(10);
    p.y = gs.stage.groundY;
    p.facing = 1;

    Sequence seq;
    for (int f = 0; f < 8; ++f) seq.frames[0].push_back(mk(BtnJump));
    for (int f = 8; f < 120; ++f) seq.frames[0].push_back(mk());
    run(gs, seq, 120);

    checkEq(gs.players[0].y, gs.stage.groundY, "landed back on the stage floor");
    check(gs.players[0].state == ActionState::Idle ||
          gs.players[0].state == ActionState::Landing,
          "recovered to a grounded state");

    // Drifting outward during the jump SHOULD fall off -- the fix must not make
    // the stage magnetic.
    GameState drift = makeMatch(1);
    drift.players[0].x = drift.stage.platformRight - fxi(10);
    drift.players[0].y = drift.stage.groundY;

    Sequence out;
    for (int f = 0; f < 8; ++f) out.frames[0].push_back(mk(BtnJump, 99));
    for (int f = 8; f < 90; ++f) out.frames[0].push_back(mk(0, 99));
    run(drift, out, 90);
    check(drift.players[0].y > drift.stage.groundY,
          "drifting outward off a jump still falls");
}

void testLedgeStatesSurviveWalkOffCheck() {
    section("ledge and knockdown states are exempt from the walk-off check");

    // Hanging on a ledge is OFF the platform by definition -- x sits outside
    // platformRight. The walk-off check must not reinterpret that as walking off,
    // or catching a ledge would immediately drop you.
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.state = ActionState::Airborne;
    p.x = gs.stage.platformRight + fxi(10);
    p.y = gs.stage.groundY + fxi(20);
    p.selfVelY = fx_ratio(10, 10);
    p.facing = -1;   // facing the stage: required to catch the right ledge

    Sequence seq;
    for (int f = 0; f < 60; ++f) seq.frames[0].push_back(mk());
    run(gs, seq, 60);

    check(gs.players[0].state == ActionState::LedgeHang,
          "still hanging after 60 frames (not dropped by the walk-off check)");
    check(gs.players[0].x > gs.stage.platformRight,
          "hang position is legitimately outside the platform");
}

// --- Hitlag -----------------------------------------------------------------

// --- SDI ---------------------------------------------------------------------

// Land a heavy hit (side smash, long freeze) and feed the DEFENDER a stick pattern
// during hitlag. `flickPeriod` 0 means never flick; otherwise flick every N frames
// with neutral in between, which is what re-flicking actually looks like.
struct SDIResult {
    bool  hit = false;
    int   lag = 0;
    fx    displacement = 0;
    uint8_t nudges = 0;
};

SDIResult sdiTrial(int flickPeriod, int8_t flickX, int8_t flickY) {
    GameState gs = makeMatch(2);
    gs.players[0].x = fxi(500);
    gs.players[0].facing = 1;
    gs.players[1].x = fxi(540);
    gs.players[1].damage = fxi(60);

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](uint16_t aBtn, int8_t aStick, int8_t dx, int8_t dy) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(aBtn, aStick);
        cur[1] = mk(0, dx, dy);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    tick(BtnAttack, 99, 0, 0);   // flick + attack = side smash, long hitlag
    for (int f = 0; f < 40 && gs.players[1].hitlagFrames == 0; ++f) {
        tick(0, 0, 0, 0);
    }

    SDIResult r;
    if (gs.players[1].hitlagFrames == 0) return r;
    r.hit = true;
    r.lag = gs.players[1].hitlagFrames;

    const fx startX = gs.players[1].x;
    int f = 0;
    while (gs.players[1].hitlagFrames > 0 && f < 60) {
        const bool doFlick = (flickPeriod > 0 && (f % flickPeriod) == 0);
        tick(0, 0, doFlick ? flickX : 0, doFlick ? flickY : 0);
        ++f;
    }
    r.displacement = gs.players[1].x - startX;
    r.nudges = gs.players[1].sdiNudges;
    return r;
}

void testSDIShiftsPosition() {
    section("SDI shifts position during hitlag");

    using namespace config;

    // SDI adds directly to POSITION rather than velocity (their
    // ftCo_Damage_OnEveryHitlag writes cur_pos). That is what makes it read as an
    // instant displacement instead of drift, and it is only possible because
    // hitlag freezes the world first.
    const SDIResult none = sdiTrial(0, 0, 0);
    const SDIResult flicking = sdiTrial(2, -99, 0);

    check(none.hit && flicking.hit, "both trials connected");
    checkEq(none.nudges, 0, "no flick -> no SDI");
    check(flicking.nudges > 0, "re-flicking earns SDI nudges");

    // Flicking away from the launch must reduce net displacement.
    check(flicking.displacement < none.displacement,
          "SDI against the launch direction reduces displacement");
}

void testSDIRequiresReflicking() {
    section("SDI requires re-flicking, not a held stick");

    using namespace config;

    // One flick buys exactly one nudge -- both stick timers saturate on success.
    // Multi-SDI therefore demands genuine re-flicking mid-freeze, which is what
    // makes it a technique rather than a reward for leaning on the stick.
    const SDIResult fast = sdiTrial(2, -99, 0);
    const SDIResult slow = sdiTrial(4, -99, 0);

    check(fast.nudges > slow.nudges,
          "faster re-flicking earns more nudges than slower");
    check(slow.nudges > 0, "slow re-flicking still earns some");

    // A stick HELD from before the hit must earn nothing: no fresh crossing.
    GameState gs = makeMatch(2);
    gs.players[0].x = fxi(500);
    gs.players[0].facing = 1;
    gs.players[1].x = fxi(526);

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](uint16_t aBtn) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(aBtn);
        cur[1] = mk(0, -99);      // defender holds left the whole time
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    tick(BtnAttack);
    for (int f = 0; f < 20 && gs.players[1].hitlagFrames == 0; ++f) tick(0);
    check(gs.players[1].hitlagFrames > 0, "hit connected");

    const fx frozenX = gs.players[1].x;
    const uint8_t lag = gs.players[1].hitlagFrames;
    for (int f = 0; f + 1 < lag; ++f) tick(0);

    checkEq(gs.players[1].sdiNudges, 0, "a stick held from before the hit gets NO SDI");
    checkEq(gs.players[1].x, frozenX, "position stays frozen without a fresh flick");
}

void testSDINudgeCap() {
    section("SDI is capped per freeze");

    using namespace config;

    // Without a cap, a long freeze could be ridden across the stage by mashing.
    const SDIResult mashed = sdiTrial(1, -99, 0);
    check(mashed.hit, "connected");
    check(mashed.nudges <= kSDI.maxNudgesPerHitlag,
          "nudges never exceed the per-freeze cap");

    // The budget resets per hit, so a second hit offers fresh SDI.
    check(kSDI.maxNudgesPerHitlag > 0, "SDI is enabled");
    check(kSDI.maxNudgesPerHitlag < 20, "the cap is a real limit, not a formality");
}

void testSDIRespectsMagnitudeAndDirection() {
    section("SDI gates on stick magnitude and works on both axes");

    using namespace config;

    // Below the magnitude threshold, nothing happens -- a light tilt is not SDI.
    const int8_t weak = static_cast<int8_t>(
        fx_to_int(fx_mul(kSDI.minStickMag, fxi(config::kStickRange))) - 15);
    const SDIResult tooWeak = sdiTrial(2, weak, 0);
    check(tooWeak.hit, "connected");
    checkEq(tooWeak.nudges, 0, "a sub-threshold deflection earns no SDI");

    // Full deflection works.
    const SDIResult strong = sdiTrial(2, -99, 0);
    check(strong.nudges > 0, "full deflection earns SDI");

    // Vertical SDI works too -- the magnitude gate is on the VECTOR, and either
    // axis flicking fresh qualifies.
    const SDIResult vertical = sdiTrial(2, 0, -99);
    check(vertical.nudges > 0, "vertical-only flicks earn SDI");

    // A diagonal qualifies even when neither axis alone clears the threshold,
    // because the gate is a vector magnitude rather than per-axis.
    const int8_t diag = static_cast<int8_t>(
        fx_to_int(fx_mul(kSDI.minStickMag, fxi(config::kStickRange))) - 5);
    const SDIResult diagonal = sdiTrial(2, static_cast<int8_t>(-diag), diag);
    check(diagonal.nudges > 0,
          "a diagonal clears the VECTOR magnitude gate even if each axis is under it");
}

void testSDIOnlyDuringHitlag() {
    section("SDI only applies during hitlag");

    using namespace config;

    // Outside a freeze, flicking must move you through normal movement rules --
    // never by teleporting position. SDI is a hitlag-only mechanic.
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.state = ActionState::Airborne;
    p.x = fxi(600);
    p.y = gs.stage.groundY - fxi(400);
    checkEq(p.hitlagFrames, 0, "not in hitlag");

    Input prev[tf::kMaxPlayers] = {};
    const fx startX = p.x;
    for (int f = 0; f < 6; ++f) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(0, (f % 2 == 0) ? -99 : 0);   // repeated flicks
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    }
    checkEq(gs.players[0].sdiNudges, 0, "no SDI nudges outside hitlag");

    // It should still have drifted normally -- flicking is a legitimate air input.
    check(gs.players[0].x != startX, "normal air drift still applies");
}

void testHitlagFreezesBothFighters() {
    section("hitlag freezes both fighters on contact");

    using namespace config;

    GameState gs = makeMatch(2);
    gs.players[0].x = fxi(500);
    gs.players[0].facing = 1;
    gs.players[1].x = fxi(526);

    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](uint16_t b) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(b);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };

    tick(BtnAttack);
    // Run to the connect frame.
    int connectFrame = -1;
    for (int f = 0; f < 20; ++f) {
        tick(0);
        if (gs.players[1].hitlagFrames > 0) { connectFrame = f; break; }
    }
    check(connectFrame >= 0, "attack connected and set hitlag");
    if (connectFrame < 0) return;

    check(gs.players[0].hitlagFrames > 0, "ATTACKER is frozen too");
    checkEq(gs.players[0].hitlagFrames, gs.players[1].hitlagFrames,
            "both fighters freeze for the SAME number of frames");

    // Knockback is assigned but position must not move while frozen. That ordering
    // is what makes a hit read as an impact rather than a teleport.
    check(gs.players[1].kbVelX != 0, "knockback is assigned at contact");
    const fx frozenX = gs.players[1].x;
    const uint8_t lag = gs.players[1].hitlagFrames;

    // lag - 1 iterations: the countdown runs at the TOP of step(), so the frame
    // that takes the timer to zero is also the frame movement resumes. Looping the
    // full `lag` times would include that release frame and read as a failure.
    for (int f = 0; f + 1 < lag; ++f) {
        tick(0);
        checkEq(gs.players[1].x, frozenX, "defender does not move during hitlag");
    }
    checkEq(gs.players[1].hitlagFrames, 1, "one freeze frame remains");

    // The release frame: the launch finally happens.
    tick(0);
    checkEq(gs.players[1].hitlagFrames, 0, "freeze expired");
    check(gs.players[1].x != frozenX, "defender moves once hitlag expires");
}

void testHitlagIsOrderIndependent() {
    section("hitlag length does not depend on player index");

    using namespace config;

    // REGRESSION: the countdown originally lived inside the per-player loop. A hit
    // resolved during player 0's turn set BOTH timers, then player 1 immediately
    // decremented their own in the same frame -- losing a frame purely for being a
    // higher slot index. The asymmetry reversed if the slots swapped, which is
    // exactly the kind of unfairness that looks fine locally and shows up online.
    auto freezeLengths = [](int attackerSlot, int defenderSlot) {
        GameState gs = makeMatch(2);
        gs.players[attackerSlot].x = fxi(500);
        gs.players[attackerSlot].facing = 1;
        gs.players[defenderSlot].x = fxi(526);

        Input prev[tf::kMaxPlayers] = {};
        for (int f = 0; f < 20; ++f) {
            Input cur[tf::kMaxPlayers] = {};
            cur[attackerSlot] = mk(f == 0 ? BtnAttack : 0);
            step(gs, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
            if (gs.players[defenderSlot].hitlagFrames > 0) break;
        }
        return std::pair<int, int>{gs.players[attackerSlot].hitlagFrames,
                                   gs.players[defenderSlot].hitlagFrames};
    };

    const auto fwd = freezeLengths(0, 1);
    checkEq(fwd.first, fwd.second, "P0 attacking P1: equal freezes");

    const auto rev = freezeLengths(1, 0);
    checkEq(rev.first, rev.second, "P1 attacking P0: equal freezes");
    checkEq(fwd.first, rev.first, "freeze length is the same either direction");
}

void testHitlagScalesWithDamage() {
    section("hitlag scales with move damage");

    using namespace config;

    // A bigger hit should freeze longer -- that coupling is what makes visual
    // weight track commitment.
    const uint8_t weak = [] {
        GameState gs = makeMatch(2);
        gs.players[0].x = fxi(500);
        gs.players[0].facing = 1;
        gs.players[1].x = fxi(526);
        Input prev[tf::kMaxPlayers] = {};
        for (int f = 0; f < 20; ++f) {
            Input cur[tf::kMaxPlayers] = {};
            cur[0] = mk(f == 0 ? BtnAttack : 0);   // jab
            step(gs, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
            if (gs.players[1].hitlagFrames > 0) break;
        }
        return gs.players[1].hitlagFrames;
    }();

    const uint8_t strong = [] {
        GameState gs = makeMatch(2);
        gs.players[0].x = fxi(500);
        gs.players[0].facing = 1;
        gs.players[1].x = fxi(540);
        Input prev[tf::kMaxPlayers] = {};
        // Flick + attack = side smash, the heaviest ground move.
        for (int f = 0; f < 30; ++f) {
            Input cur[tf::kMaxPlayers] = {};
            cur[0] = mk(f == 0 ? BtnAttack : 0, f == 0 ? 99 : 0);
            step(gs, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
            if (gs.players[1].hitlagFrames > 0) break;
        }
        return gs.players[1].hitlagFrames;
    }();

    check(weak > 0, "jab produces hitlag");
    check(strong > 0, "smash produces hitlag");
    check(strong > weak, "the heavier move freezes longer");

    // Clamped at both ends so nothing becomes uninteractable.
    check(weak >= kHitlag.minFrames, "hitlag respects the minimum");
    check(strong <= kHitlag.maxFrames, "hitlag respects the maximum");
}

// --- Fast-fall requires a flick ---------------------------------------------

void testFastFallRequiresFlick() {
    section("fast-fall needs a fresh flick, not a held stick");

    using namespace config;

    // ftCommon_CheckFallFast gates on the stick-tilt RECENCY timer, the same one
    // that separates smashes from tilts. Holding down through a rise must not
    // auto-fast-fall the instant you begin descending.
    GameState held = makeMatch(1);
    {
        Player& p = held.players[0];
        p.state = ActionState::Airborne;
        p.y = held.stage.groundY - fxi(400);
        p.selfVelY = fx_ratio(-40, 10);   // rising
    }
    Sequence holdDown;
    for (int f = 0; f < 60; ++f) holdDown.frames[0].push_back(mk(0, 0, 99));
    run(held, holdDown, 60);
    check(!held.players[0].fastFalling,
          "holding down through the arc does NOT fast-fall");

    // A fresh flick while already descending DOES fast-fall.
    GameState flick = makeMatch(1);
    {
        Player& p = flick.players[0];
        p.state = ActionState::Airborne;
        p.y = flick.stage.groundY - fxi(400);
        p.selfVelY = fx_ratio(20, 10);    // already descending
    }
    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](const Input& i0) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = i0;
        step(flick, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };
    tick(mk());                 // neutral first, so the next frame is a crossing
    tick(mk(0, 0, 99));         // flick down
    check(flick.players[0].fastFalling, "a fresh downward flick fast-falls");

    // Rising must still reject it, flick or not.
    GameState rising = makeMatch(1);
    {
        Player& p = rising.players[0];
        p.state = ActionState::Airborne;
        p.y = rising.stage.groundY - fxi(400);
        p.selfVelY = fx_ratio(-40, 10);
    }
    Input rp[tf::kMaxPlayers] = {};
    Input r1[tf::kMaxPlayers] = {};
    step(rising, r1, rp);
    std::memcpy(rp, r1, sizeof(r1));
    Input r2[tf::kMaxPlayers] = {};
    r2[0] = mk(0, 0, 99);
    step(rising, r2, rp);
    check(!rising.players[0].fastFalling, "cannot fast-fall while rising");
}

// --- Per-aerial landing lag -------------------------------------------------

void testPerAerialLandingLag() {
    section("each aerial has its own landing lag");

    using namespace config;

    // Melee has five separate values (landingairn_lag, landingairf_lag, ...) so
    // "which aerial is safe to land with" is a real decision. One shared number
    // makes every aerial equally safe and flattens that choice.
    const auto& atk = kScoutAttacks;
    check(atk[ATK_AIR_NEUTRAL].landingLag > 0, "nair has its own lag");
    check(atk[ATK_AIR_DOWN].landingLag > atk[ATK_AIR_NEUTRAL].landingLag,
          "the spike is more punishing to land with than nair");
    check(atk[ATK_AIR_FORWARD].landingLag > atk[ATK_AIR_NEUTRAL].landingLag,
          "fair is more committal than nair");

    // Ground moves leave it zero, meaning "use the character default".
    checkEq(atk[ATK_JAB].landingLag, 0, "ground moves do not set aerial lag");

    // Per-character too: the heavy character is punished harder for the same move.
    check(kBruiserAttacks[ATK_AIR_DOWN].landingLag >
              kScoutAttacks[ATK_AIR_DOWN].landingLag,
          "bruiser suffers more landing lag on the same aerial");

    // Measured end-to-end: land a nair and confirm the lag matches its own value.
    GameState gs = makeMatch(1);
    {
        Player& p = gs.players[0];
        p.state = ActionState::Airborne;
        p.y = gs.stage.groundY - fxi(60);
        p.selfVelY = fx_ratio(20, 10);
    }
    Input prev[tf::kMaxPlayers] = {};
    auto tick = [&](uint16_t b) {
        Input cur[tf::kMaxPlayers] = {};
        cur[0] = mk(b);
        step(gs, cur, prev);
        std::memcpy(prev, cur, sizeof(cur));
    };
    tick(BtnAttack);   // neutral aerial
    for (int f = 0; f < 200 && gs.players[0].state != ActionState::Landing; ++f) {
        tick(0);
    }
    check(gs.players[0].state == ActionState::Landing, "landed out of the aerial");
    checkEq(gs.players[0].landingLag, atk[ATK_AIR_NEUTRAL].landingLag,
            "latched lag is the aerial's own value");
}

// --- Air drift --------------------------------------------------------------

void testAirDriftIsProportional() {
    section("air drift scales with stick magnitude");

    using namespace config;

    // ftCommon_8007D28C uses stick * stickMul + sign(stick) * base -- a
    // proportional term plus a flat floor. A single constant makes drift
    // all-or-nothing past the deadzone, losing the analog nuance that matters most
    // when drifting out of a launch.
    auto driftAfter = [](int8_t stickX, int frames) {
        GameState gs = makeMatch(1);
        Player& p = gs.players[0];
        p.state = ActionState::Airborne;
        p.y = gs.stage.groundY - fxi(500);
        p.selfVelX = 0;
        p.selfVelY = 0;

        Sequence seq;
        for (int f = 0; f < frames; ++f) seq.frames[0].push_back(mk(0, stickX));
        run(gs, seq, frames);
        return gs.players[0].selfVelX;
    };

    const fx light = driftAfter(40, 6);
    const fx full = driftAfter(99, 6);
    check(light > 0, "light stick produces some drift");
    check(full > light, "full deflection drifts harder than a light tilt");

    // The flat base means even a minimal deflection past the deadzone moves you --
    // drift is never zero once you are asking for it.
    const fx minimal = driftAfter(static_cast<int8_t>(kStick.deadzone + 2), 6);
    check(minimal > 0, "a minimal deflection still drifts (flat base term)");
}

void testAirDriftHardCeiling() {
    section("hard horizontal ceiling is separate from the drift target");

    using namespace config;
    const auto& air = kFighters[CHAR_SCOUT].air;

    // air_max_horizontal_velocity is a distinct, higher cap than air_drift_max:
    // momentum carried from a launch or a wavedash may EXCEED what you could
    // accelerate to, but never the ceiling. Collapsing the two clamps carried
    // momentum down to drift pace, which kills every momentum-based technique.
    check(air.maxHorizontal > air.maxSpeed,
          "the ceiling is higher than the drift target");

    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.state = ActionState::Airborne;
    p.y = gs.stage.groundY - fxi(500);
    p.selfVelX = air.maxHorizontal + fxi(4);   // over the ceiling

    Sequence seq;
    for (int f = 0; f < 10; ++f) seq.frames[0].push_back(mk(0, 99));
    run(gs, seq, 10);

    check(gs.players[0].selfVelX <= air.maxHorizontal,
          "velocity is clamped to the hard ceiling");

    // Carried momentum above the drift target must NOT be dragged down to it while
    // still holding that direction.
    GameState carry = makeMatch(1);
    carry.players[0].state = ActionState::Airborne;
    carry.players[0].y = carry.stage.groundY - fxi(500);
    carry.players[0].selfVelX = air.maxSpeed + fx_ratio(8, 10);
    Sequence hold;
    for (int f = 0; f < 4; ++f) hold.frames[0].push_back(mk(0, 99));
    run(carry, hold, 4);
    check(carry.players[0].selfVelX > air.maxSpeed,
          "momentum above the drift target is preserved, not clamped to it");
}

void testAirTurnaroundIsResponsive() {
    section("turning around in the air is exempt from the target clamp");

    using namespace config;

    // Their velocity-approach function skips the clamp when vel * accel < 0 --
    // pushing against your own momentum applies full acceleration. That exemption
    // is why reversals feel responsive rather than mushy.
    GameState gs = makeMatch(1);
    Player& p = gs.players[0];
    p.state = ActionState::Airborne;
    p.y = gs.stage.groundY - fxi(500);
    p.selfVelX = fx_ratio(-30, 10);          // moving LEFT fast

    const fx before = p.selfVelX;
    Sequence seq;
    for (int f = 0; f < 3; ++f) seq.frames[0].push_back(mk(0, 99));   // push RIGHT
    run(gs, seq, 3);

    check(gs.players[0].selfVelX > before, "reversal changes velocity rightward");
    // The change should exceed what a clamped approach to the target would give in
    // three frames, since turning around bypasses the clamp entirely.
    const fx delta = gs.players[0].selfVelX - before;
    check(delta > 0, "turnaround makes progress against existing momentum");
}

void testBlastZoneKills() {
    section("blast zone costs a stock");

    GameState gs = makeMatch(1);
    gs.players[0].x = gs.stage.blastRight + fxi(10);   // already out of bounds
    gs.players[0].state = ActionState::Airborne;
    const int16_t before = gs.players[0].stocks;

    Sequence seq;
    seq.frames[0].push_back(mk());
    run(gs, seq, 1);

    check(gs.players[0].state == ActionState::Dead, "out of bounds kills");
    checkEq(gs.players[0].stocks, before - 1, "a stock is spent");
    checkEq(fx_to_int(gs.players[0].damage), 0, "damage resets on death");
}

void testStockOutDeactivates() {
    section("losing the last stock deactivates the player");

    // This is the signal the arena watches for to eject someone: the sim clears
    // `active` once the final stock is spent.
    GameState gs = makeMatch(1);
    gs.players[0].stocks = 1;
    gs.players[0].x = gs.stage.blastRight + fxi(10);
    gs.players[0].state = ActionState::Airborne;

    Sequence seq;
    for (int f = 0; f < 120; ++f) seq.frames[0].push_back(mk());
    run(gs, seq, 120);

    checkEq(gs.players[0].stocks, 0, "last stock spent");
    check(!gs.players[0].active, "player deactivated -> arena will eject them");
}

void testKOFullHeal() {
    section("KO credit grants a FULL HEAL");

    // The core mechanic of the arena mode: landing a KO wipes the killer's damage
    // back to 0%, so holding a stage is possible but only while you keep
    // converting. Staged at the ledge because a jab deliberately does not kill
    // from center stage.
    GameState gs = makeMatch(2);
    gs.players[0].x = gs.stage.platformRight - fxi(30);
    gs.players[0].facing = 1;
    gs.players[0].damage = fxi(85);          // killer is badly damaged
    gs.players[1].x = gs.stage.platformRight - fxi(10);
    gs.players[1].damage = fxi(300);         // victim launches far

    Sequence seq;
    seq.frames[0].push_back(mk(BtnAttack));
    for (int f = 1; f < 400; ++f) seq.frames[0].push_back(mk());
    run(gs, seq, 400);

    check(gs.players[1].stocks < 4, "victim lost a stock");
    check(gs.players[0].pendingKOs > 0, "KO credited to the attacker");
    checkEq(fx_to_int(gs.players[0].damage), 0, "attacker FULL HEALED to 0%");
}

}  // namespace

int main() {
    std::printf("=== tier-fighter simulation tests ===\n");

    testFixedPoint();
    testTrig();
    testDeterminism();
    testJumping();
    testFastFall();
    testWavedashEmerges();
    testDashCommitsToRun();
    testDashDanceHoldsPosition();
    testCannotDanceOutOfRun();
    testTurnaroundCostsFrames();
    testWalkAccelerates();
    testRunAccelerationTapers();
    testGroundMoveIsPerCharacter();
    testGroundFrictionIsNotIcy();
    testOneAirDodgePerAirtime();
    testHelplessAfterAirDodge();
    testHelplessCanStillGrabLedge();
    testHelplessLandingCostsMore();
    testLCancel();
    testGroundAttackMatrix();
    testAirAttackMatrix();
    testSmashCharge();
    testAttackProfiles();
    testKnockbackFormula();
    testAirDriftDuringHitstun();
    testInvulnerability();
    testHitboxDistanceNoOverflow();
    testAttackReachIsBounded();
    testPerCharacterStats();
    testPerCharacterAttacks();
    testDefenderBodyDecidesHurtbox();
    testInvalidCharIdIsSafe();
    testImpactSpeedDecidesOutcome();
    testTechBeatsKnockdown();
    testTechLockoutPreventsMashing();
    testGetUpOptions();
    testGetUpInputIsBuffered();
    testKnockdownIsPerCharacter();
    testBounceCannotLoop();
    testLedgeGrab();
    testLedgeRequiresFacing();
    testHitstunCannotAutoGrabLedge();
    testLedgeHangBehavior();
    testLedgeOptions();
    testLedgeSlowQuickByDamage();
    testLedgeClimbPlacesOnStage();
    testHitOffLedgeSetsCooldown();
    testLedgeIsPerCharacter();
    testLedgeCannotStallForever();
    testWalkOffEdge();
    testJumpNearEdgeLandsNormally();
    testLedgeStatesSurviveWalkOffCheck();
    testSDIShiftsPosition();
    testSDIRequiresReflicking();
    testSDINudgeCap();
    testSDIRespectsMagnitudeAndDirection();
    testSDIOnlyDuringHitlag();
    testHitlagFreezesBothFighters();
    testHitlagIsOrderIndependent();
    testHitlagScalesWithDamage();
    testFastFallRequiresFlick();
    testPerAerialLandingLag();
    testAirDriftIsProportional();
    testAirDriftHardCeiling();
    testAirTurnaroundIsResponsive();
    testBlastZoneKills();
    testStockOutDeactivates();
    testKOFullHeal();

    std::printf("\n=====================================\n");
    if (g_failures == 0) {
        std::printf("PASS: all %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAIL: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
