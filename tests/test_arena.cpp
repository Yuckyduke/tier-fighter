#include "sim/arena.h"
#include "sim/config.h"
#include "sim/state.h"

#include <cstdio>
#include <vector>

// Tests for the arena: on-demand stage creation, KO scoring, streaks, and routing.
//
// The population behavior matters most. A fixed set of rooms with a small
// playerbase produces empty stages and no fights -- these tests assert that stage
// count actually tracks population instead.

using namespace tf;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { std::printf("  FAIL: %s\n", what); ++g_failures; }
}

void checkEq(int32_t got, int32_t want, const char* what) {
    ++g_checks;
    if (got != want) {
        std::printf("  FAIL: %s (got %d, want %d)\n", what, got, want);
        ++g_failures;
    }
}

void section(const char* name) { std::printf("\n[%s]\n", name); }

const auto& A = config::kArena;
const auto& S = config::kScoring;

void idleStep(Arena& arena, int frames = 1) {
    std::vector<Input> in(kMaxTrackedPlayers), prev(kMaxTrackedPlayers);
    for (int f = 0; f < frames; ++f) {
        arena.step(in.data(), prev.data(), kMaxTrackedPlayers);
    }
}

// Award a KO the way the simulation does: bump pendingKOs on the killer.
bool giveKO(Arena& arena, uint32_t playerId, int count = 1) {
    const PlayerRecord* rec = arena.player(playerId);
    if (!rec || rec->stageIndex < 0) return false;
    auto& s = const_cast<StageInstance&>(arena.stage(rec->stageIndex));
    s.gs.players[rec->slot].pendingKOs += static_cast<uint8_t>(count);
    return true;
}

// Knock a player off: deactivate them, which is what the sim does on stock-out.
bool knockOff(Arena& arena, uint32_t playerId) {
    const PlayerRecord* rec = arena.player(playerId);
    if (!rec || rec->stageIndex < 0) return false;
    auto& s = const_cast<StageInstance&>(arena.stage(rec->stageIndex));
    s.gs.players[rec->slot].active = false;
    return true;
}

int countEvents(const Arena& arena, EventKind kind) {
    int n = 0;
    for (int i = 0; i < arena.eventCount(); ++i) {
        if (arena.events()[i].kind == kind) ++n;
    }
    return n;
}

// --- Stage creation scales with population ----------------------------------

void testStagesCreatedOnDemand() {
    section("stages are created on demand, not pre-allocated");

    Arena arena;
    checkEq(arena.liveStageCount(), 0, "empty arena has ZERO stages");

    check(arena.joinPlayer(1), "first player joined");
    checkEq(arena.liveStageCount(), 1, "first join creates one stage");

    // Filling a stage must not create more.
    for (uint32_t id = 2; id <= static_cast<uint32_t>(A.playersPerStage); ++id) {
        check(arena.joinPlayer(id), "player joined existing stage");
    }
    checkEq(arena.liveStageCount(), 1, "stage fills before a second is created");

    // Overflow creates a second -- creation happens exactly when every stage is full.
    check(arena.joinPlayer(999), "overflow player joined");
    checkEq(arena.liveStageCount(), 2, "full stage triggers a new one");

    // The headline property: stage count tracks population.
    Arena big;
    for (uint32_t id = 0; id < 30; ++id) big.joinPlayer(id);
    const int expected = (30 + A.playersPerStage - 1) / A.playersPerStage;
    checkEq(big.liveStageCount(), expected, "30 players produce ceil(30/N) stages");
    checkEq(big.totalPopulation(), 30, "all players accounted for");
}

void testFullestStageFirst() {
    section("arrivals pack into the FULLEST stage (chaos, not tidy duels)");

    // Two stages: one full, one with a single player. A new arrival must join the
    // partially-filled one rather than creating a third.
    Arena arena;
    for (uint32_t id = 0; id < static_cast<uint32_t>(A.playersPerStage) + 1; ++id) {
        arena.joinPlayer(id);
    }
    checkEq(arena.liveStageCount(), 2, "two stages exist");

    const int before = arena.liveStageCount();
    arena.joinPlayer(500);
    checkEq(arena.liveStageCount(), before, "arrival joins existing stage");

    // With three non-full stages at different populations, arrivals must choose
    // the fullest. Packing is what makes rooms feel busy; spreading out would
    // produce lots of quiet 1v1s.
    Arena packed;
    for (uint32_t id = 0; id < 3; ++id) packed.joinPlayer(id);
    // One stage with 3. Force two more stages by filling and overflowing.
    for (uint32_t id = 3; id < static_cast<uint32_t>(A.playersPerStage); ++id) {
        packed.joinPlayer(id);
    }
    // Stage 1 is now full; next joiner opens stage 2.
    packed.joinPlayer(100);
    packed.joinPlayer(101);   // should join stage 2 (pop 1), making it 2

    const PlayerRecord* r100 = packed.player(100);
    const PlayerRecord* r101 = packed.player(101);
    check(r100 && r101, "both overflow players placed");
    if (r100 && r101) {
        checkEq(static_cast<int>(r101->stageId), static_cast<int>(r100->stageId),
                "second overflow joins the same non-full stage, not a new one");
    }
}

// --- KO scoring -------------------------------------------------------------

void testKOScoring() {
    section("KO scoring");

    Arena arena;
    for (uint32_t id = 0; id < 3; ++id) arena.joinPlayer(id);

    check(giveKO(arena, 0), "KO credited to player 0");
    idleStep(arena);

    const PlayerRecord* r = arena.player(0);
    check(r != nullptr, "record exists");
    if (r) {
        checkEq(r->kos, 1, "KO counted");
        checkEq(r->currentStreak, 1, "streak incremented");
        checkEq(r->bestStreak, 1, "best streak tracked");
        checkEq(r->score, S.pointsPerKO, "score awarded");
        checkEq(r->netDelta(), 1, "net delta reflects the KO");
    }
    checkEq(countEvents(arena, EventKind::KO), 1, "KO event emitted");

    // pendingKOs must be consumed, or the same KO would score every frame.
    idleStep(arena, 5);
    r = arena.player(0);
    if (r) checkEq(r->kos, 1, "KO is not double-counted across frames");

    // Multiple KOs in one frame must all score (counter, not a flag).
    check(giveKO(arena, 1, 2), "two KOs credited in one frame");
    idleStep(arena);
    const PlayerRecord* r1 = arena.player(1);
    if (r1) {
        checkEq(r1->kos, 2, "both KOs counted");
        checkEq(r1->score, 2 * S.pointsPerKO, "both KOs scored");
    }
}

void testStreakBonus() {
    section("streak bonus pays at every multiple of the threshold");

    Arena arena;
    for (uint32_t id = 0; id < 3; ++id) arena.joinPlayer(id);

    // One short of the threshold: no bonus yet.
    for (int i = 0; i < S.streakThreshold - 1; ++i) {
        giveKO(arena, 0);
        idleStep(arena);
    }
    const PlayerRecord* r = arena.player(0);
    if (r) {
        checkEq(r->currentStreak, S.streakThreshold - 1, "streak just below threshold");
        checkEq(r->streakBonusesEarned, 0, "no bonus before the threshold");
        checkEq(r->score, (S.streakThreshold - 1) * S.pointsPerKO,
                "score is KOs only, no bonus");
    }

    // Hitting the threshold awards the bonus.
    giveKO(arena, 0);
    idleStep(arena);
    r = arena.player(0);
    if (r) {
        checkEq(r->currentStreak, S.streakThreshold, "streak reached threshold");
        checkEq(r->streakBonusesEarned, 1, "bonus awarded at the threshold");
        checkEq(r->score, S.streakThreshold * S.pointsPerKO + S.streakBonus,
                "score includes the bonus");
    }
    checkEq(countEvents(arena, EventKind::StreakBonus), 1, "bonus event emitted");

    // Reaching 2x the threshold awards a second bonus -- sustained dominance keeps
    // paying rather than capping out after one award.
    for (int i = 0; i < S.streakThreshold; ++i) {
        giveKO(arena, 0);
        idleStep(arena);
    }
    r = arena.player(0);
    if (r) {
        checkEq(r->currentStreak, 2 * S.streakThreshold, "streak at 2x threshold");
        checkEq(r->streakBonusesEarned, 2, "second bonus awarded");
    }
}

void testKnockoffResetsStreak() {
    section("being knocked off resets the streak");

    Arena arena;
    for (uint32_t id = 0; id < 8; ++id) arena.joinPlayer(id);

    // Build a streak.
    for (int i = 0; i < 3; ++i) { giveKO(arena, 0); idleStep(arena); }
    const PlayerRecord* r = arena.player(0);
    check(r && r->currentStreak == 3, "streak of 3 built");
    const int scoreBeforeKnockoff = r ? r->score : 0;

    // Get knocked off.
    check(knockOff(arena, 0), "player 0 knocked off");
    idleStep(arena);

    r = arena.player(0);
    check(r != nullptr, "player still in the arena after being knocked off");
    if (r) {
        checkEq(r->currentStreak, 0, "streak RESET by the knock-off");
        checkEq(r->bestStreak, 3, "best streak is retained");
        checkEq(r->knockoffs, 1, "knock-off counted");
        checkEq(r->score, scoreBeforeKnockoff + S.pointsPerKnockoff,
                "knock-off penalty applied");
        check(r->status == PlayerStatus::Fighting, "still fighting, not eliminated");
        check(r->stageIndex >= 0, "immediately placed on a stage, no waiting");
    }
    checkEq(countEvents(arena, EventKind::Knockoff), 1, "knockoff event emitted");
}

// --- Routing ----------------------------------------------------------------

void testKnockedOffMovesToDifferentStage() {
    section("knocked off -> routed to a DIFFERENT stage");

    // Enough players for several stages, so an alternative always exists.
    Arena arena;
    for (uint32_t id = 0; id < 20; ++id) arena.joinPlayer(id);
    check(arena.liveStageCount() >= 2, "multiple stages exist");

    const PlayerRecord* before = arena.player(0);
    check(before != nullptr, "subject exists");
    if (!before) return;
    const uint32_t leftStage = before->stageId;

    check(knockOff(arena, 0), "knocked off");
    idleStep(arena);

    const PlayerRecord* after = arena.player(0);
    check(after != nullptr, "survived the knock-off");
    if (after) {
        check(after->stageIndex >= 0, "placed on a stage");
        checkEq(static_cast<int>(after->lastStageId), static_cast<int>(leftStage),
                "remembers the stage just left");
        check(after->stageId != leftStage,
              "routed to a DIFFERENT stage than the one just left");
    }
}

void testSingleStageFallback() {
    section("returns to the same stage only when it is the only option");

    // Two players, one stage. A knock-off has nowhere else to go, so reusing the
    // stage is correct rather than stranding them.
    Arena arena;
    arena.joinPlayer(0);
    arena.joinPlayer(1);
    checkEq(arena.liveStageCount(), 1, "one stage only");

    check(knockOff(arena, 0), "knocked off");
    idleStep(arena);

    const PlayerRecord* after = arena.player(0);
    check(after != nullptr, "player survived");
    if (after) {
        check(after->stageIndex >= 0, "player is on a stage, not stranded");
        check(after->status == PlayerStatus::Fighting, "still fighting");
    }
    // A fresh stage may be created rather than reusing -- either is acceptable, the
    // requirement is only that the player is never left without a stage.
    check(arena.liveStageCount() >= 1, "at least one stage remains");
}

// --- Lifecycle --------------------------------------------------------------

void testStagesCloseWhenEmpty() {
    section("stages close when they empty out");

    Arena arena;
    arena.joinPlayer(0);
    arena.joinPlayer(1);
    checkEq(arena.liveStageCount(), 1, "one stage");

    arena.removePlayer(0);
    arena.removePlayer(1);
    checkEq(arena.liveStageCount(), 0, "stage closed when the last player left");
    check(arena.player(0) == nullptr, "removed player is gone");
    checkEq(arena.totalPopulation(), 0, "population is zero");
}

void testLonelyStageConsolidates() {
    section("lonely stages consolidate after a grace period");

    Arena arena;
    // Fill one stage, then add one more so a second stage holds a single player.
    for (uint32_t id = 0; id < static_cast<uint32_t>(A.playersPerStage) + 1; ++id) {
        arena.joinPlayer(id);
    }
    checkEq(arena.liveStageCount(), 2, "two stages, one holding a single player");

    // Nothing should move before the grace period expires.
    idleStep(arena, A.lonelyGraceFrames / 2);
    checkEq(arena.liveStageCount(), 2, "no consolidation during grace period");

    // The other stage is full, so there's nowhere to merge to. Must not thrash or
    // strand the lone player.
    idleStep(arena, A.lonelyGraceFrames + 2);
    const PlayerRecord* lonely =
        arena.player(static_cast<uint32_t>(A.playersPerStage));
    check(lonely != nullptr, "lone player still tracked");
    if (lonely) check(lonely->stageIndex >= 0, "lone player still on a stage");

    // Free a slot; consolidation should now merge them.
    arena.removePlayer(0);
    idleStep(arena, A.lonelyGraceFrames + 2);
    checkEq(arena.liveStageCount(), 1, "lone player merged, empty stage closed");
}

// --- Leaderboard ------------------------------------------------------------

void testLeaderboard() {
    section("leaderboard ranking");

    Arena arena;
    for (uint32_t id = 0; id < 5; ++id) arena.joinPlayer(id);

    // Give distinct KO counts so ranking is unambiguous.
    for (int i = 0; i < 5; ++i) { giveKO(arena, 2); idleStep(arena); }
    for (int i = 0; i < 3; ++i) { giveKO(arena, 4); idleStep(arena); }
    for (int i = 0; i < 1; ++i) { giveKO(arena, 0); idleStep(arena); }

    PlayerRecord board[kMaxTrackedPlayers];
    const int n = arena.leaderboard(board, kMaxTrackedPlayers);
    checkEq(n, 5, "all active players on the board");

    // Player 2 has 5 KOs plus a streak bonus, so leads.
    if (n >= 3) {
        checkEq(static_cast<int>(board[0].id), 2, "highest scorer is first");
        checkEq(static_cast<int>(board[1].id), 4, "second highest is second");
        checkEq(static_cast<int>(board[2].id), 0, "third highest is third");
    }

    // Scores must be non-increasing down the board.
    bool ordered = true;
    for (int i = 1; i < n; ++i) {
        if (board[i - 1].score < board[i].score) ordered = false;
    }
    check(ordered, "board is sorted by score descending");

    // Ties must break deterministically by id, or two machines could rank equal
    // players differently.
    Arena tied;
    for (uint32_t id = 0; id < 4; ++id) tied.joinPlayer(id);
    for (uint32_t id = 0; id < 4; ++id) { giveKO(tied, id); }
    idleStep(tied);

    PlayerRecord tiedBoard[kMaxTrackedPlayers];
    const int tn = tied.leaderboard(tiedBoard, kMaxTrackedPlayers);
    checkEq(tn, 4, "all tied players on the board");
    bool idOrdered = true;
    for (int i = 1; i < tn; ++i) {
        if (tiedBoard[i - 1].score == tiedBoard[i].score &&
            tiedBoard[i - 1].id > tiedBoard[i].id) {
            idOrdered = false;
        }
    }
    check(idOrdered, "equal scores break ties by id (deterministic)");

    // maxEntries must be respected.
    PlayerRecord top2[2];
    checkEq(arena.leaderboard(top2, 2), 2, "leaderboard honors maxEntries");
    checkEq(arena.leaderboard(nullptr, 0), 0, "zero maxEntries returns zero");
}

// --- Determinism ------------------------------------------------------------

void testArenaDeterminism() {
    section("arena bookkeeping is deterministic");

    // Identical join / KO / knockoff sequences must produce identical arena state.
    // If routing depended on iteration incidentals, two peers would place players
    // differently and desync.
    auto runScenario = [](std::vector<int>& log) {
        Arena arena;
        for (uint32_t id = 0; id < 16; ++id) arena.joinPlayer(id);
        for (int round = 0; round < 8; ++round) {
            for (uint32_t id = 0; id < 16; ++id) {
                // Deterministic pattern -- no RNG, no clock.
                if ((id + round) % 4 == 0) giveKO(arena, id);
                else if ((id + round) % 7 == 0) knockOff(arena, id);
            }
            idleStep(arena);
        }
        for (uint32_t id = 0; id < 16; ++id) {
            const PlayerRecord* r = arena.player(id);
            log.push_back(r ? r->score : -1);
            log.push_back(r ? r->kos : -1);
            log.push_back(r ? r->knockoffs : -1);
            log.push_back(r ? static_cast<int>(r->stageId) : -1);
            log.push_back(r ? r->currentStreak : -1);
        }
        log.push_back(arena.liveStageCount());
    };

    std::vector<int> a, b;
    runScenario(a);
    runScenario(b);
    check(a == b, "identical sequences produce identical arena state");
    check(!a.empty(), "state log is non-empty");
}

void testPopulationConserved() {
    section("population is conserved across churn");

    Arena arena;
    for (uint32_t id = 0; id < 17; ++id) arena.joinPlayer(id);
    checkEq(arena.totalPopulation(), 17, "all joiners counted");

    // Churn: KOs and knockoffs together.
    for (int round = 0; round < 10; ++round) {
        for (uint32_t id = 0; id < 17; ++id) {
            if (id % 3 == 0) giveKO(arena, id);
            if (id % 5 == 0) knockOff(arena, id);
        }
        idleStep(arena);
    }

    checkEq(arena.totalPopulation(), 17, "nobody lost or duplicated");

    // Sum of stage populations must equal the tracked total.
    int summed = 0;
    for (int i = 0; i < arena.stageCapacity(); ++i) {
        const StageInstance& s = arena.stage(i);
        if (s.live) summed += s.population();
    }
    checkEq(summed, 17, "stage populations sum to the total");

    // Every stage's slot bookkeeping must agree with the simulation state.
    for (int i = 0; i < arena.stageCapacity(); ++i) {
        const StageInstance& s = arena.stage(i);
        if (!s.live) continue;
        check(s.population() > 0, "no live stage is empty");
        check(s.population() <= A.playersPerStage, "no stage exceeds capacity");
    }
}

}  // namespace

int main() {
    std::printf("=== arena tests ===\n");
    std::printf("config: playersPerStage=%d stocksPerStage=%d streakThreshold=%d\n",
                A.playersPerStage, A.stocksPerStage, S.streakThreshold);

    testStagesCreatedOnDemand();
    testFullestStageFirst();
    testKOScoring();
    testStreakBonus();
    testKnockoffResetsStreak();
    testKnockedOffMovesToDifferentStage();
    testSingleStageFallback();
    testStagesCloseWhenEmpty();
    testLonelyStageConsolidates();
    testLeaderboard();
    testArenaDeterminism();
    testPopulationConserved();

    std::printf("\n===================\n");
    if (g_failures == 0) {
        std::printf("PASS: all %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAIL: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
