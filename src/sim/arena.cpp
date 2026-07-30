#include "arena.h"

namespace tf {

namespace {

const config::Arena& A = config::kArena;
const config::Scoring& S = config::kScoring;

}  // namespace

Arena::Arena() {
    // Nothing pre-allocated. The first joiner creates the first stage -- that's
    // what makes stage count track population instead of a fixed depth.
}

// --- Lookup ------------------------------------------------------------------

const PlayerRecord* Arena::player(uint32_t playerId) const {
    for (uint32_t i = 0; i < playerCount_; ++i) {
        if (players_[i].id == playerId &&
            players_[i].status != PlayerStatus::Absent) {
            return &players_[i];
        }
    }
    return nullptr;
}

PlayerRecord* Arena::mutablePlayer(uint32_t playerId) {
    for (uint32_t i = 0; i < playerCount_; ++i) {
        if (players_[i].id == playerId &&
            players_[i].status != PlayerStatus::Absent) {
            return &players_[i];
        }
    }
    return nullptr;
}

int Arena::liveStageCount() const {
    int n = 0;
    for (const StageInstance& s : stages_) if (s.live) ++n;
    return n;
}

int Arena::totalPopulation() const {
    int n = 0;
    for (uint32_t i = 0; i < playerCount_; ++i) {
        if (players_[i].status == PlayerStatus::Fighting) ++n;
    }
    return n;
}

void Arena::emit(EventKind k, uint32_t playerId, uint32_t stageId, int value) {
    if (eventCount_ < static_cast<int>(kMaxTrackedPlayers * 2)) {
        events_[eventCount_++] = ArenaEvent{k, playerId, stageId, value};
    }
}

// --- Stage lifecycle ---------------------------------------------------------

// FULLEST non-full stage, skipping the one the player just left.
//
// Fullest-first is deliberate and it's what creates the chaos: players pack into
// busy rooms instead of spreading out. The opposite policy (emptiest-first) would
// balance populations neatly and produce lots of quiet duels -- exactly the wrong
// feel for a game about being swarmed while defending a stage.
int Arena::findStageForArrival(uint32_t avoidStageId) const {
    int best = -1;
    int bestPop = -1;

    for (int i = 0; i < kMaxStages; ++i) {
        const StageInstance& s = stages_[i];
        if (!s.live || s.full()) continue;
        if (s.id == avoidStageId) continue;   // no immediate rematch

        const int pop = s.population();
        if (pop > bestPop) { bestPop = pop; best = i; }
    }

    // Nothing available except the stage they just left: take it rather than
    // stranding them. This is the "unless it's the only stage" fallback.
    if (best < 0 && avoidStageId != 0) {
        for (int i = 0; i < kMaxStages; ++i) {
            const StageInstance& s = stages_[i];
            if (s.live && !s.full() && s.id == avoidStageId) return i;
        }
    }
    return best;
}

int Arena::allocateStage() {
    for (int i = 0; i < kMaxStages; ++i) {
        if (stages_[i].live) continue;
        StageInstance& s = stages_[i];
        s = StageInstance{};            // clear any prior match state
        s.live = true;
        s.id = nextStageId_++;
        emit(EventKind::StageCreated, 0, s.id, 0);
        return i;
    }
    return -1;   // at capacity
}

void Arena::closeStage(int index) {
    StageInstance& s = stages_[index];
    if (!s.live) return;
    emit(EventKind::StageClosed, 0, s.id, 0);
    s.live = false;
    for (int i = 0; i < kPlayersPerStage; ++i) s.slotOccupied[i] = false;
}

bool Arena::placeOnStage(uint32_t playerId, int stageIndex) {
    StageInstance& s = stages_[stageIndex];
    int slot = -1;
    for (int i = 0; i < kPlayersPerStage; ++i) {
        if (!s.slotOccupied[i]) { slot = i; break; }
    }
    if (slot < 0) return false;

    s.slotOccupied[slot] = true;
    s.slotPlayer[slot] = playerId;

    // Arrivals drop in from above, briefly invulnerable -- the "zoning in" moment,
    // and it stops new arrivals being free kills for whoever is already there.
    Player& p = s.gs.players[slot];
    p = Player{};
    p.active = true;
    p.stocks = static_cast<int16_t>(A.stocksPerStage);
    p.x = (s.gs.stage.platformLeft + s.gs.stage.platformRight) / 2;
    p.y = s.gs.stage.groundY - A.arrivalHeight;
    p.state = ActionState::Airborne;
    p.invulnFrames = static_cast<uint16_t>(A.arrivalInvulnFrames);

    PlayerRecord* rec = mutablePlayer(playerId);
    if (rec) {
        rec->stageIndex = stageIndex;
        rec->stageId = s.id;
        rec->slot = slot;
    }
    return true;
}

// Free a player's slot. `closeIfEmpty` is false when the caller is about to place
// them somewhere else and wants to defer the close decision.
void Arena::detachFromStage(PlayerRecord& rec, bool closeIfEmpty) {
    if (rec.stageIndex < 0) return;
    StageInstance& s = stages_[rec.stageIndex];

    if (rec.slot >= 0 && rec.slot < kPlayersPerStage) {
        s.slotOccupied[rec.slot] = false;
        s.gs.players[rec.slot] = Player{};
    }
    rec.lastStageId = s.id;   // remembered so they don't bounce straight back

    const int idx = rec.stageIndex;
    rec.stageIndex = -1;
    rec.slot = -1;

    if (closeIfEmpty && s.population() == 0) closeStage(idx);
}

// --- Movement ----------------------------------------------------------------

// Route an ejected player to a different stage, creating one if every existing
// stage is full. Stage creation happens on demand, exactly when there's nowhere
// to put someone.
bool Arena::relocate(PlayerRecord& rec) {
    const uint32_t avoid = rec.stageId;
    const int fromStage = rec.stageIndex;

    int dest = findStageForArrival(avoid);
    if (dest < 0) {
        dest = allocateStage();
        if (dest < 0) return false;   // arena at capacity
    }

    // Detach without closing: the old stage is only closed after the player is
    // safely placed, so a failed placement can never leave them belonging nowhere.
    detachFromStage(rec, /*closeIfEmpty=*/false);
    const bool ok = placeOnStage(rec.id, dest);

    if (fromStage >= 0 && fromStage != dest && stages_[fromStage].live &&
        stages_[fromStage].population() == 0) {
        closeStage(fromStage);
    }
    return ok;
}

bool Arena::joinPlayer(uint32_t playerId) {
    if (player(playerId) != nullptr) return false;   // already here

    PlayerRecord* rec = nullptr;
    for (uint32_t i = 0; i < playerCount_; ++i) {
        if (players_[i].id == playerId) { rec = &players_[i]; break; }
    }
    if (!rec) {
        if (playerCount_ >= kMaxTrackedPlayers) return false;
        rec = &players_[playerCount_++];
    }

    *rec = PlayerRecord{};
    rec->id = playerId;
    rec->status = PlayerStatus::Fighting;

    int dest = findStageForArrival(0);
    if (dest < 0) {
        dest = allocateStage();
        if (dest < 0) { rec->status = PlayerStatus::Absent; return false; }
    }
    return placeOnStage(playerId, dest);
}

void Arena::removePlayer(uint32_t playerId) {
    PlayerRecord* rec = mutablePlayer(playerId);
    if (!rec) return;
    detachFromStage(*rec, /*closeIfEmpty=*/true);
    rec->status = PlayerStatus::Absent;
}

// --- Step --------------------------------------------------------------------

void Arena::step(const Input* inputsByPlayer, const Input* prevInputsByPlayer,
                 uint32_t inputCount) {
    eventCount_ = 0;

    // 1. Advance every live stage, ascending index order so the arena as a whole
    //    stays deterministic.
    for (int i = 0; i < kMaxStages; ++i) {
        StageInstance& s = stages_[i];
        if (!s.live) continue;

        Input in[kMaxPlayers];
        Input prev[kMaxPlayers];
        for (int slot = 0; slot < kPlayersPerStage; ++slot) {
            if (!s.slotOccupied[slot]) continue;
            const uint32_t pid = s.slotPlayer[slot];
            if (pid < inputCount) {
                in[slot] = inputsByPlayer[pid];
                prev[slot] = prevInputsByPlayer[pid];
            }
        }
        tf::step(s.gs, in, prev);
    }

    // 2. Score KOs. The simulation credits them into Player::pendingKOs and applies
    //    the full heal; the arena just converts them into points and streaks.
    //    Scoring runs BEFORE knockoffs so a trade -- both players dying on the same
    //    frame -- still pays the killer.
    for (int i = 0; i < kMaxStages; ++i) {
        StageInstance& s = stages_[i];
        if (!s.live) continue;
        for (int slot = 0; slot < kPlayersPerStage; ++slot) {
            if (!s.slotOccupied[slot]) continue;
            Player& p = s.gs.players[slot];
            if (p.pendingKOs == 0) continue;

            const int kos = p.pendingKOs;
            p.pendingKOs = 0;   // consumed

            PlayerRecord* rec = mutablePlayer(s.slotPlayer[slot]);
            if (!rec) continue;

            for (int k = 0; k < kos; ++k) {
                rec->kos++;
                rec->currentStreak++;
                if (rec->currentStreak > rec->bestStreak) {
                    rec->bestStreak = rec->currentStreak;
                }
                rec->score += S.pointsPerKO;
                emit(EventKind::KO, rec->id, s.id, rec->currentStreak);

                // The bonus pays at EVERY multiple of the threshold, so a 15-KO run
                // earns it three times. Sustained dominance keeps paying instead of
                // capping out after a single award.
                if (S.streakThreshold > 0 &&
                    rec->currentStreak % S.streakThreshold == 0) {
                    rec->streakBonusesEarned++;
                    rec->score += S.streakBonus;
                    emit(EventKind::StreakBonus, rec->id, s.id, S.streakBonus);
                }
            }
        }
    }

    // 3. Collect knockoffs BEFORE mutating anything, so the outcome doesn't depend
    //    on mutation order and nobody can be moved twice in one frame.
    //
    //    The sim clears `active` once a player's last stock is spent; with
    //    stocksPerStage == 1 that means a single knock-off ejects you.
    uint32_t ejected[kMaxTrackedPlayers];
    int ejectedCount = 0;

    for (int i = 0; i < kMaxStages; ++i) {
        StageInstance& s = stages_[i];
        if (!s.live) continue;
        for (int slot = 0; slot < kPlayersPerStage; ++slot) {
            if (!s.slotOccupied[slot]) continue;
            if (s.gs.players[slot].active) continue;
            if (ejectedCount < static_cast<int>(kMaxTrackedPlayers)) {
                ejected[ejectedCount++] = s.slotPlayer[slot];
            }
        }
    }

    // 4. Apply them: score, reset streak, route to a DIFFERENT stage. No
    //    elimination and no waiting -- you're back in a fight immediately.
    for (int i = 0; i < ejectedCount; ++i) {
        PlayerRecord* rec = mutablePlayer(ejected[i]);
        if (!rec || rec->status != PlayerStatus::Fighting) continue;

        rec->knockoffs++;
        rec->currentStreak = 0;   // the whole reason streak is tracked separately
        rec->score += S.pointsPerKnockoff;
        const uint32_t leftStage = rec->stageId;

        relocate(*rec);
        emit(EventKind::Knockoff, rec->id, leftStage, 0);
    }

    // 5. Consolidate stages that have gone quiet. The grace period matters: don't
    //    yank someone the instant their opponent is ejected, since new arrivals
    //    may be seconds away.
    for (int i = 0; i < kMaxStages; ++i) {
        StageInstance& s = stages_[i];
        if (!s.live) continue;

        const int pop = s.population();
        if (pop == 0) { closeStage(i); continue; }

        if (pop < A.minPlayersPerStage) {
            s.lonelyFrames++;
        } else {
            s.lonelyFrames = 0;
            continue;
        }
        if (s.lonelyFrames < A.lonelyGraceFrames) continue;

        // Grace expired. Is there a better-populated stage with room?
        int target = -1;
        int bestPop = kPlayersPerStage + 1;
        for (int j = 0; j < kMaxStages; ++j) {
            if (j == i) continue;
            const StageInstance& o = stages_[j];
            if (!o.live || o.full()) continue;
            const int opop = o.population();
            if (opop < bestPop) { bestPop = opop; target = j; }
        }
        if (target < 0) { s.lonelyFrames = 0; continue; }   // nowhere better

        for (int slot = 0; slot < kPlayersPerStage; ++slot) {
            if (!s.slotOccupied[slot]) continue;
            PlayerRecord* rec = mutablePlayer(s.slotPlayer[slot]);
            if (!rec) continue;

            detachFromStage(*rec, /*closeIfEmpty=*/false);
            if (placeOnStage(rec->id, target)) {
                emit(EventKind::Consolidated, rec->id, stages_[target].id, 0);
            }
        }
        closeStage(i);
    }
}

// --- Leaderboard -------------------------------------------------------------

int Arena::leaderboard(PlayerRecord* out, int maxEntries) const {
    if (maxEntries <= 0) return 0;

    int n = 0;
    for (uint32_t i = 0; i < playerCount_ && n < maxEntries; ++i) {
        if (players_[i].status == PlayerStatus::Absent) continue;
        out[n++] = players_[i];
    }

    // Insertion sort: n is small (a few hundred at most) and this is stable and
    // allocation-free, which keeps it usable from the same context as the sim.
    for (int i = 1; i < n; ++i) {
        PlayerRecord key = out[i];
        int j = i - 1;
        while (j >= 0) {
            const PlayerRecord& c = out[j];
            // Ranking policy. The trailing id comparison makes the order TOTAL, so
            // two machines with identical state always produce identical rankings.
            bool keyIsBetter = false;
            if (key.score != c.score)                    keyIsBetter = key.score > c.score;
            else if (key.netDelta() != c.netDelta())      keyIsBetter = key.netDelta() > c.netDelta();
            else if (key.bestStreak != c.bestStreak)      keyIsBetter = key.bestStreak > c.bestStreak;
            else if (key.knockoffs != c.knockoffs)        keyIsBetter = key.knockoffs < c.knockoffs;
            else                                          keyIsBetter = key.id < c.id;

            if (!keyIsBetter) break;
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = key;
    }
    return n;
}

}  // namespace tf
