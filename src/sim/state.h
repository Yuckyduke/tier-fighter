#pragma once
#include "config.h"
#include "fixed.h"
#include "input.h"
#include <cstdint>

namespace tf {

constexpr int kMaxPlayers = config::kMaxPlayers;
constexpr int kTicksPerSecond = config::kTicksPerSecond;

// Action states. Which state you're in determines what inputs you can act on --
// that's the whole grammar of a fighting game: commitment and recovery.
// Sentinel for Player::lastKilledBy: no attacker is credited. Deliberately outside
// any valid player index, so it can never be mistaken for slot 0.
constexpr uint8_t kNoAttacker = 0xFF;
// Saturation ceiling for the per-frame KO counter -- a bound, not a tunable.
constexpr uint8_t kMaxPendingKOs = 0xFE;

enum class ActionState : uint8_t {
    Idle,
    Walk,
    Dash,
    Jumpsquat,
    Airborne,
    Landing,

    // --- Ground movement, split ----------------------------------------------
    // Dash is the initial burst and is INTERRUPTIBLE -- flicking the stick
    // re-enters it, which is what makes dash-dancing possible. Run is the
    // committed sustained sprint and is NOT interruptible: leaving it requires
    // RunBrake. Turn is the standing reversal, a timed commitment rather than a
    // free flip of `facing`.
    Run,
    RunBrake,
    Turn,
    AttackGround,
    AttackAir,
    AirDodge,
    Hitstun,
    Dead,

    // --- Knockdown family ----------------------------------------------------
    // Entered when a player in hitstun hits the ground hard. Mirrors the decomp's
    // DownBound / DownWait / DownStand / Passive states.
    Bounce,      // brief airborne arc after slamming into the ground
    DownWait,    // lying on the ground, vulnerable, choosing an option
    GetUp,       // neutral stand-up
    GetUpRoll,   // roll left/right to reposition
    GetUpAttack, // rising attack; uses ATK_GETUP
    Tech,        // successfully teched -- instant recovery, brief invulnerability

    // --- Ledge family --------------------------------------------------------
    // Mirrors the decomp's Cliff* states. Their 12 collapse to 6 here because
    // slow/quick is a parameter (which frame count), not a separate state.
    LedgeHang,   // hanging on the edge, choosing an option
    LedgeClimb,  // climbing up onto the stage
    LedgeRoll,   // rolling onto the stage past the edge
    LedgeAttack, // get-up attack from the ledge; uses ATK_LEDGE
    LedgeJump,   // releasing into a jump

    // Helpless fall after an air dodge. Mirrors the decomp's FallSpecial: a
    // distinct state whose input handling has NO air-dodge entry, which is how
    // Melee limits you to one dodge per airborne period without needing a counter.
    // You can still drift and grab a ledge, but not dodge or jump again.
    FallHelpless,
};

using Stage = config::StageLayout;
using AttackData = config::AttackData;
using Fighter = config::Fighter;

// One fighter's runtime state.
//
// POD only: no pointers, no heap, no virtuals. Rollback netcode snapshots and
// restores this with a plain memcpy, so anything owning memory is forbidden.
struct Player {
    bool active = false;

    // Which character. The simulation reads ALL character constants through
    // config::kFighters[charId] and never touches a global -- so gravity, weight,
    // jump height, and the entire attack table are per-player.
    uint8_t charId = config::CHAR_SCOUT;

    fx x = 0, y = 0;

    // Velocity is DECOMPOSED, not a single vector. Melee splits a fighter's
    // motion into independent components that sum at integration time, and that
    // split is load-bearing for game feel:
    //   selfVel — your own movement: walking, jumping, air drift. You control it.
    //   kbVel   — knockback from being hit. Decays along its own vector on its
    //             own schedule, and you do NOT directly control it.
    // Keeping them separate is what lets you air-drift *while* being launched:
    // your drift accumulates in selfVel while kbVel independently decays. Collapse
    // them into one vector and that interaction disappears -- combos and recovery
    // both stop feeling right, and no amount of constant-tuning brings them back.
    fx selfVelX = 0, selfVelY = 0;
    fx kbVelX = 0, kbVelY = 0;

    // Actual motion this frame. Derived, not authoritative -- do not write.
    fx totalVelX() const { return selfVelX + kbVelX; }
    fx totalVelY() const { return selfVelY + kbVelY; }

    int8_t facing = 1; // +1 right, -1 left

    ActionState state = ActionState::Idle;
    uint16_t stateFrame = 0; // frames elapsed in the current state

    fx damage = 0; // percent; drives knockback scaling
    int16_t stocks = 4;
    uint8_t airJumps = 0;
    bool fastFalling = false;
    bool jumpHeld = false; // short-hop detection during jumpsquat

    uint16_t hitstunFrames = 0;
    uint8_t lcancelTimer = 0; // recent shield press -> halved landing lag
    // Landing lag LATCHED at touchdown. Must not be recomputed per frame: the
    // lcancel window expires during the lag itself, so re-deciding mid-Landing
    // would flip a successful L-cancel back to full lag partway through.
    uint8_t landingLag = 0;

    // --- Attack state --------------------------------------------------------
    uint8_t attackId = 0; // AttackIndex; which move is out
    uint16_t attackFrame = 0;
    bool attackConnected = false; // one hit per swing

    // Smash charging. `charging` holds the move in its last pre-hit frame while
    // the attack button is held, accumulating chargeFrames.
    bool charging = false;
    uint8_t chargeFrames = 0;

    // Frames the stick has been continuously deflected on each axis, saturating.
    // Reset to 0 on the frame it CROSSES the threshold, so a small value means
    // "just flicked" -- that recency is what separates a smash from a tilt.
    // Mirrors the decomp's x670/x671_timer_lstick_tilt fields.
    uint8_t stickHeldX = 0xFE;
    uint8_t stickHeldY = 0xFE;
    uint16_t invulnFrames = 0;
    uint16_t respawnTimer = 0;

    // Who last launched this player -- determines KO credit when they die.
    // kNoAttacker means nobody: a self-destruct earns no one a KO.
    uint8_t lastKilledBy = kNoAttacker;

    // KOs scored since the arena last read this. Consumed and cleared each frame;
    // a counter rather than a flag so two KOs in one frame both score.
    uint8_t pendingKOs = 0;

    // --- Knockdown / tech ----------------------------------------------------
    // Counts down from a shield press while in hitstun. Hitting the ground with
    // this non-zero techs the landing; hitting it at zero means knockdown.
    uint8_t techWindow = 0;
    // Set when a tech attempt expires without reaching the ground. While non-zero
    // no tech can succeed, which is what makes mashing shield a losing strategy
    // rather than the optimal one.
    uint8_t techLockout = 0;
    // Consecutive bounces this knockdown, so a slam can't bounce forever.
    uint8_t bounceCount = 0;
    // Roll direction while in GetUpRoll: -1 left, +1 right.
    int8_t rollDir = 0;
    // Frames remaining in a timed ground action (turn, run-brake). Latched on
    // entry so the duration cannot be re-derived mid-action.
    uint8_t groundActionFrames = 0;

    // Buttons pressed while a get-up option was still locked out. Without this a
    // player who mashes on knockdown has the press consumed during lockout and
    // must release and re-press -- the input would silently vanish.
    uint16_t bufferedButtons = 0;

    // --- Hitlag --------------------------------------------------------------
    // Frames of contact freeze remaining. While non-zero the player does not
    // advance: no movement, no state progression, no attack frames. BOTH fighters
    // in an exchange get one, which is what makes a hit read as an impact rather
    // than a teleport.
    uint8_t hitlagFrames = 0;

    // --- Ledge ---------------------------------------------------------------
    // Which ledge is held: -1 none, 0 left, 1 right. Stored rather than recomputed
    // so the hang position stays fixed even as the body is repositioned.
    int8_t ledgeSide = -1;
    // Counts down while hanging; at zero you are forced off (anti-stalling).
    uint16_t ledgeHangFrames = 0;
    // Regrab lockout. Non-zero means no ledge can be caught -- set on leaving the
    // ledge by ANY route, which is what stops infinite invulnerable ledge camping.
    uint8_t ledgeCooldown = 0;
    // Frame budget for the current ledge action, LATCHED on entry. Slow vs quick is
    // decided once from damage, not re-evaluated per frame (the same latching bug
    // that bit landingLag).
    uint8_t ledgeActionFrames = 0;
};

// Complete simulation state for one stage. memcpy-able: this is the rollback unit.
struct GameState {
    uint32_t tick = 0;
    Player players[kMaxPlayers];
    Stage stage;
    uint32_t rngState = 0x2545F491u; // deterministic; reserved for later use

    int livePlayers() const {
        int n = 0;
        for (const auto &p : players)
            if (p.active) ++n;
        return n;
    }
};

// The entire simulation: a pure function of (state, inputs) -> state.
// No I/O, no clock reads, no allocation, no floats. Same arguments always produce
// the same result, on any machine, which is what makes rollback possible.
void step(GameState &gs, const Input inputs[kMaxPlayers], const Input prevInputs[kMaxPlayers]);

// Deterministic checksum for desync detection. Peers compare it per frame; a
// mismatch names the exact frame that diverged.
uint32_t checksum(const GameState &gs);

const Fighter &defaultFighter();

} // namespace tf
