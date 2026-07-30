# Engineering plan

What's done, what's next, and why in that order. Updated as work lands.

**Status:** 809 checks passing. Simulation, arena, playtest client and pose editor
all work. Local only — there is no netcode, which is the single thing standing
between this and real playtesters.

---

## The critical path

Everything below is ordered by what unblocks the most. The honest summary: **the
game is mechanically deep and completely unplayable by anyone else.** Physics work
has diminishing returns until that changes.

| # | Task | Why now | Size |
|---|---|---|---|
| 1 | **Rollback netcode** | The only true blocker. Nothing else matters until two people can play. | Large |
| 2 | **Minimal front end** | Testers can't use a build that drops into a hardcoded match. | Medium |
| 3 | **Wire the arena to the client** | Stage pooling and KO scoring are tested but never driven by the renderer. | Small |
| 4 | **Playtest with 8+ humans** | The whole arena premise is unvalidated. | — |

Items 1–3 are roughly 4–6 weeks of work. Item 4 is what tells you whether any of
the rest was worth building.

---

## 1. Netcode

The simulation was built for this from the first commit: `step()` is pure, no
floats, no allocation, and `GameState` is `memcpy`-able with a static assert
enforcing it. `testDeterminism` already snapshots at frame 100, runs to 400,
rewinds, replays, and asserts bit-identical state — **that test is rollback minus
the network.**

- [ ] Input ring buffer (send/receive/predict per player)
- [ ] State snapshot ring, sized to the max rollback window
- [ ] Rollback loop: detect a late input, rewind, re-simulate
- [ ] Transport — ENet is the obvious choice (reliable UDP, battle-tested)
- [ ] Desync detection using the existing per-frame checksum
- [ ] Frame pacing / delay tuning

**Risks.** Rollback is genuinely hard to get right, and the failure mode is subtle:
it works locally and desyncs online, minutes in, with no useful stack trace. The
checksum exists precisely to name the frame that diverged. Budget debugging time.

The sim/arena split helps here — the arena is authoritative server logic and needs
no rollback, only the per-stage simulation does.

## 2. Front end

No menus exist. `tf_play` drops straight into a hardcoded 2-player match.

- [ ] Title screen
- [ ] Host / join (an IP entry box is fine)
- [ ] Character select (two characters exist: Scout, Bruiser)
- [ ] Results / leaderboard screen — the leaderboard is already implemented in
      `arena.cpp`, just never displayed
- [ ] Pause and quit

Raygui (raylib's companion UI library) handles all of this. No art needed.

## 3. Arena integration

`src/sim/arena.cpp` implements stage pooling, on-demand creation, KO scoring,
streak bonuses, and routing — 90 tests cover it — but the client runs a single bare
`GameState`. Connecting them turns this from a sparring tool into the actual game.

- [ ] Client drives `Arena::step` instead of `tf::step`
- [ ] Render the stage the local player is on
- [ ] Show the routing transition when ejected to a new stage
- [ ] Display streak and score from `PlayerRecord`

---

## Done

Chronologically, most recent first. Each of these is pushed and tested.

- **Stick figures in the game** — poses drawn from joint angles, `K` toggles back to
  boxes. Attack poses key off `attackFrame` so held charges freeze correctly.
- **Pose system** — 42 states, standalone editor (`tf_poses`), 20 transition
  sequences, generated blending. See [POSES.md](POSES.md).
- **Grab, pummel, throws** — hold timer scales with victim damage; two independent
  mash drains; throws guaranteed once started.
- **Shield, break, dizzy, rolls, spotdodge** — health with three flows; shieldstun
  from the largest hit while health loss uses the sum.
- **SDI** — position shift during hitlag, requires re-flicking.
- **Dash / Run split, turn frames, walk acceleration** — unlocks dash-dancing.
- **Hitlag, per-aerial landing lag, proportional air drift, flick-gated fast-fall**
- **Ledges** — grab requires facing, hang timeout, five exits, regrab cooldown.
- **Knockdown / tech** — branches on knockback surviving at impact.
- **Full attack matrix** — jab, 3 tilts, 3 chargeable smashes, 5 aerials.
- **Per-character fighters** — everything hangs off `charId`.
- **Arena** — stage pool, KO scoring with full heal, streaks, routing, leaderboard.
- **Deterministic simulation** — fixed-point, baked trig, rollback-ready.

---

## Deferred, with reasons

Not "forgotten" — actively decided against for now.

| Item | Why deferred |
|---|---|
| **More characters** | Two is enough to keep the per-character plumbing honest. Mirror matches are fine for playtesting. |
| **Sound** | Zero effect on whether the loop is fun. Cheap to add later from libraries. |
| **Real art** | Stick figures solve legibility. Sprites are months of work and only worth it once the game is proven. |
| **Powershield** | Its own timer web plus reflect hitboxes — a feature in its own right. |
| **Tiered tower** | Tried and cut: needs a large concurrent population or the upper floors sit empty. See [NOTES.md](NOTES.md). |
| **Items** | Would be one mechanic parameterised by item type, but adds nothing to the core loop. |

---

## Remaining physics gaps

From the audit in [PHYSICS.md](PHYSICS.md). None of these block playtesting; they're
depth, not blockers. `tools/coverage.py` tracks the numbers.

- [ ] Crouch + crouch-cancelling (`kb_squat_mul` — reduces knockback taken)
- [ ] Tap jump (flick up to jump; no such input exists)
- [ ] Multi-hit attacks (needs a per-attack hit list, not the current bool)
- [ ] Attack clank (two attacks colliding cancel)
- [ ] Wall / ceiling collision, wall tech, wall jump — needs stages with walls
- [ ] ECB diamond instead of AABB — affects ledge-grab feel
- [ ] `gr_vel` + slope projection — prerequisite for sloped stages
- [ ] Rapid jab / jab combos
- [ ] Angled tilts (their side tilt has five aim variants)
- [ ] Ledge teeter, edge slip, platform pass-through

---

## Pose polish

The poses are first-pass, written from reasoning rather than observation. Expect
some to read badly — [POSES.md](POSES.md) covers the workflow.

- [ ] Review all 42 in `tf_poses` sequence mode
- [ ] Fix the ones that snap or read wrongly
- [ ] Check held smash charges — the wind-up pose is on screen up to 60 frames
- [ ] Tune the figure-to-hitbox height ratio (currently 1.6×, a guess)

---

## Known open questions

Things where the answer isn't obvious and playtesting should decide.

- **Does the arena loop actually work with humans?** Proven in tests, never with
  people. The full-heal-on-KO mechanic in particular could feel oppressive rather
  than exciting.
- **Is the scoring balanced?** KO is +100, knock-off is −40, and a dominant player
  compounds via the full heal. Score gaps will widen fast.
- **Is one character enough for a session?** Testers will engage with movement and
  the loop, but there's no progression to hold them past ten minutes.
- **Keyboard vs controller.** Several techniques depend on analog stick angles a
  keyboard can only express as a 45° diagonal. Untested with a pad.
