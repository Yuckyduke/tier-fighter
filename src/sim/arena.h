#pragma once
#include "config.h"
#include "state.h"
#include <cstdint>

namespace tf {

// The arena: a flat pool of stages, scored by KOs.
//
// The loop:
//   - Each STAGE is a persistent room, created on demand and closed when empty.
//     Stage count tracks population -- nothing is pre-allocated.
//   - Score a KO: points, streak increment, and a FULL HEAL back to 0%. That heal
//     is the core mechanic. It makes holding a stage possible but never safe: you
//     can survive a stream of arrivals only as long as you keep converting. Take
//     damage without scoring and you become as launchable as anyone else.
//   - Get knocked off: you're EJECTED and routed to a DIFFERENT stage. Streak
//     resets. You keep playing immediately -- there's no elimination and no
//     waiting.
//
// There are no levels or tiers. A tiered structure needs a large concurrent population
// for its upper floors to be populated at all -- at 20 players online, whoever
// climbs highest sits alone. A flat pool behaves identically at 4 players or 400.
// Tiers can layer on later once population justifies them.
//
// Two routing rules produce the intended chaos:
//   1. An ejected player never returns to the stage they just left, unless it's
//      genuinely the only option. Fresh opponents every time.
//   2. Arrivals fill the FULLEST non-full stage. Packing stages rather than
//      spreading players evenly is what makes rooms feel busy and contested -- the
//      opposite policy yields lots of quiet duels.

constexpr int kMaxStages         = config::kMaxStages;
constexpr int kPlayersPerStage   = kMaxPlayers;
constexpr int kMaxTrackedPlayers = config::kMaxTrackedPlayers;

struct StageInstance {
    bool      live = false;
    uint32_t  id = 0;            // stable identity, distinct from array index
    GameState gs;
    uint32_t  slotPlayer[kPlayersPerStage] = {};
    bool      slotOccupied[kPlayersPerStage] = {};

    // Frames spent below minPlayersPerStage; drives consolidation.
    int lonelyFrames = 0;

    int population() const {
        int n = 0;
        for (bool o : slotOccupied) if (o) ++n;
        return n;
    }
    bool full() const { return population() >= config::kArena.playersPerStage; }
};

enum class PlayerStatus : uint8_t {
    Absent,     // not in the arena
    Fighting,   // on a stage
};

struct PlayerRecord {
    uint32_t     id = 0;
    PlayerStatus status = PlayerStatus::Absent;
    int          stageIndex = -1;
    uint32_t     stageId = 0;
    uint32_t     lastStageId = 0;   // just-left stage; avoid routing back to it
    int          slot = -1;

    // --- Leaderboard stats ---------------------------------------------------
    int kos = 0;                  // lifetime KOs scored
    int knockoffs = 0;            // lifetime times knocked off
    int currentStreak = 0;        // consecutive KOs; being knocked off resets it
    int bestStreak = 0;
    int streakBonusesEarned = 0;
    int score = 0;

    // Net KOs. The base leaderboard measure.
    int netDelta() const { return kos - knockoffs; }
};

enum class EventKind : uint8_t {
    KO, Knockoff, StreakBonus, StageCreated, StageClosed, Consolidated,
};

// Emitted by step() so the caller can react: kill feed, notify clients, animate
// the transition, log for balance analysis.
struct ArenaEvent {
    EventKind kind;
    uint32_t  playerId;   // 0 for stage-only events
    uint32_t  stageId;
    int       value;      // streak length for KO, bonus points for StreakBonus
};

class Arena {
public:
    Arena();

    bool joinPlayer(uint32_t playerId);
    void removePlayer(uint32_t playerId);

    // Advance every live stage, then score KOs, resolve knockoffs, and handle
    // stage creation and consolidation. Inputs are indexed by global player id.
    void step(const Input* inputsByPlayer, const Input* prevInputsByPlayer,
              uint32_t inputCount);

    // --- Queries -------------------------------------------------------------
    const PlayerRecord*  player(uint32_t playerId) const;
    const StageInstance& stage(int index) const { return stages_[index]; }
    int  stageCapacity() const { return kMaxStages; }
    int  liveStageCount() const;
    int  totalPopulation() const;

    const ArenaEvent* events() const { return events_; }
    int eventCount() const { return eventCount_; }

    // Fills `out` with up to `maxEntries` records ranked best-first; returns how
    // many were written. Ranking lives here so the policy can change without
    // touching match logic.
    //
    // Order: score, then net delta, then best streak, then fewest knockoffs, then
    // id. The id tiebreak makes the ordering TOTAL and therefore deterministic --
    // without it, equal-scoring players could rank differently on two machines.
    int leaderboard(PlayerRecord* out, int maxEntries) const;

private:
    int  findStageForArrival(uint32_t avoidStageId) const;
    int  allocateStage();
    void closeStage(int index);
    bool placeOnStage(uint32_t playerId, int stageIndex);
    bool relocate(PlayerRecord& rec);
    void detachFromStage(PlayerRecord& rec, bool closeIfEmpty);
    PlayerRecord* mutablePlayer(uint32_t playerId);
    void emit(EventKind k, uint32_t playerId, uint32_t stageId, int value);

    StageInstance stages_[kMaxStages];
    PlayerRecord  players_[kMaxTrackedPlayers];
    uint32_t      playerCount_ = 0;
    uint32_t      nextStageId_ = 1;

    ArenaEvent events_[kMaxTrackedPlayers * 2];
    int        eventCount_ = 0;
};

}  // namespace tf
