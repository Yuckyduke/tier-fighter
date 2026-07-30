# tier-fighter

A platform fighter built for online play, with a rollback-ready deterministic
simulation and an endless multi-stage arena.

Melee-inspired. Written in C++20, no engine.

## The idea

Most platform fighters are match-based: you queue, you fight, someone wins, you
queue again. This one is continuous.

- Every **stage** is a persistent room. Stages are created on demand and closed
  when they empty, so stage count tracks the live population — nothing is
  pre-allocated.
- **Score a KO** → points, a streak increment, and a **full heal back to 0%**.
- **Get knocked off** → you're ejected and routed to a *different* stage. Your
  streak resets. No elimination, no waiting; you're in a new fight immediately.

The full heal is the load-bearing mechanic. It makes holding a stage possible but
never safe: you can survive a stream of arrivals only as long as you keep
converting. Take damage without scoring and you're as launchable as anyone else.
Dominance is rewarded; camping isn't.

## Quickstart

Clone to playing, in full:

```sh
# 1. Dependencies  (macOS)
brew install cmake raylib
#                 (Debian/Ubuntu)
# sudo apt install cmake libraylib-dev

# 2. Build
cmake -S . -B build
cmake --build build

# 3. Play
./build/tf_play
```

The build step is required before `./build/tf_play` exists — `build/` is
generated, not checked in.

Needs a C++20 compiler and CMake 3.20+. [raylib](https://www.raylib.com/) is
optional: without it the tests still build, you just don't get the playable
client (CMake will warn and skip the `tf_play` target).

## Running

```sh
./build/tf_play          # the game: local 2-player sparring
./build/tf_poses         # pose editor -- animation work, no gameplay
./build/tf_tests         # simulation tests
./build/tf_arena_tests   # arena / matchmaking tests
```

After changing anything in `src/`, rebuild and relaunch:

```sh
cmake --build build && ./build/tf_play
```

`tf_play` is a **local sparring build** — two players at one keyboard, one stage,
no arena routing and no netcode. The arena described above (stage pooling, KO
scoring, streaks, routing) is implemented and tested in `src/sim/arena.cpp` but
not yet wired to the client.

### Controls

| | Player 1 | Player 2 |
|---|---|---|
| Move | `WASD` | Arrow keys |
| Jump | `Space` | `Right Shift` |
| Attack | `F` | `.` |
| Shield / air dodge | `G` | `/` |

Gamepads work too, and are strongly preferred — several techniques depend on
analog stick angles that a keyboard can only express as a 45° diagonal.

**Debug keys:** `K` stick figures / boxes · `H` hitboxes · `P` pause · `N` step one
frame · `Alt` slow-mo · `R` reset · `Tab` hide help

### Looking at the poses

Fighters are stick figures drawn from joint-angle data. There's a dedicated editor:

```sh
./build/tf_poses
```

Press `M` for **sequence mode**, which chains states back-to-back (wavedash,
dash-dance, knockdown recovery) so you can see transitions rather than isolated
poses — transitions are where problems actually show up. `[` and `]` page through.

Full workflow, including how to edit a pose and paste it back:
**[POSES.md](POSES.md)**.

### Techniques to try

- **Short hop** — release jump during the 4-frame jumpsquat
- **Wavedash** — jump, then air dodge shallow down-and-forward just above the ground
- **L-cancel** — press shield just before landing an aerial (halves landing lag)
- **Fast fall** — flick the stick down while already descending
- **Tech** — press shield just before hitting the ground in hitstun
- **Smash vs tilt** — flick the stick and attack together for a smash; hold a
  direction first and you get a tilt. Hold attack to charge a smash.

## Architecture

```
src/sim/          the simulation — zero dependencies, compiles into a headless server
  fixed.h         Q16.16 fixed-point math
  trig.h/cpp      baked sine table
  input.h         one frame of input (the network + rollback unit)
  config.h        every tunable value, grouped by concern
  state.h         Player + GameState
  sim.cpp         step(state, inputs) -> state
  arena.h/cpp     stage pool, KO scoring, streaks, routing, leaderboard
src/render/       presentation: playtest client, stick-figure poses, pose editor
tests/            809 checks
tools/coverage.py physics coverage audit
```

### The invariant everything rests on

`step()` is a **pure function**. No I/O, no clock reads, no allocation, no floats,
no randomness. The same inputs always produce bit-identical output on any machine.

That's not tidiness — it's the prerequisite for rollback netcode, which works by
rewinding and re-simulating past frames. `testDeterminism` snapshots at frame 100,
runs to 400, rewinds, replays, and asserts the state matches bit-for-bit. **That
test is rollback, minus the network.** It also statically asserts `GameState` stays
trivially copyable, so adding a `std::vector` to `Player` fails the build rather
than silently corrupting saves.

Three consequences worth knowing about:

**Fixed-point, not floats.** Floats aren't bit-identical across compilers,
optimization levels, or architectures. One differing bit compounds into a visible
desync. `-ffast-math` is explicitly disabled and must never creep in.

**Baked trig.** `std::sin` isn't specified to be bit-identical across platforms, so
the sine table is generated once and hardcoded.

**No numeric literals in `sim.cpp`.** Every tunable lives in `config.h`. Tuning
those numbers *is* the game design; the formulas in `sim.cpp` are structure.

### Velocity is decomposed

`Player` carries `selfVel` (your own movement) and `kbVel` (knockback) separately.
They sum at integration time.

This matters more than it looks: it's what lets you air-drift *while* being
launched. Your drift accumulates in `selfVel` while `kbVel` decays independently.
Collapse them into one vector and that interaction disappears — combos and recovery
both stop feeling right, and no amount of constant-tuning brings them back.

### The sim / arena split

The simulation knows nothing about scoring or matchmaking. It reports KOs via
`Player::pendingKOs` and stock-outs via `active = false`; the arena interprets
them. Either can be rewritten without touching the other.

## Design notes

Longer write-ups live alongside the code:

- **[NOTES.md](NOTES.md)** — design decisions and why the tiered-tower idea was cut
- **[PHYSICS.md](PHYSICS.md)** — physics systems, what's implemented, what's missing
- **[STATES.md](STATES.md)** — action-state design
- **[POSES.md](POSES.md)** — stick-figure poses and the pose editor
- **[PLAN.md](PLAN.md)** — engineering plan: what's done, what's next, what's deferred

One idea worth pulling out, because it shaped everything else:

**Write rules, not techniques.** Wavedashing isn't implemented anywhere —
`grep -i wavedash src/` returns only comments. It *emerges* from three independent
rules: an air dodge sets velocity along the stick, landing zeroes only *vertical*
velocity, and ground friction merely decays horizontal velocity. The technique
lives in the gap between them.

That's verified rather than assumed: `testWavedashEmerges` was checked by
deliberately breaking rule 2, and the wavedash died immediately. Code a "wavedash
feature" and you get one scripted-feeling technique. Code the momentum rules right
and players discover things you didn't design.

## Status

**809 checks passing.** The simulation, arena, playtest client, and pose editor work.

Implemented: full attack matrix (jab, tilts, chargeable smashes, five aerials);
per-character fighters; knockdown, tech and get-up options; ledges; hitlag and SDI;
shield with health, break and dizzy; rolls and spotdodge; grabs, pummel and four
throws; separate dash and run states with turnaround frames; two-tier
speed-dependent ground friction.

Not built yet:

- **Netcode.** The simulation is built for it but no transport exists. This is the
  big one — everything else is playable locally only.
- **Front end.** No menus, so `tf_play` drops straight into a hardcoded match.
- **Arena wiring.** Stage pooling and KO scoring are implemented and tested but
  the client still runs a single bare match.

**[PLAN.md](PLAN.md)** tracks what's left, in priority order, with task checklists
and the reasoning behind the ordering. It's the place to look for "what should I work
on next?".
- **Art.** Fighters are procedural stick figures rather than sprites — 42 action
  states would otherwise mean 40+ hand-drawn animations per character. Poses are
  authored as joint angles in a dedicated editor; see [POSES.md](POSES.md).

`tools/coverage.py` reports how much of the physics surface is covered, and is
honest about what it can't determine.

## Relationship to Melee

This is an original codebase — it contains no Nintendo code or assets, and every
constant in `config.h` is my own. The [Melee decompilation](https://github.com/doldecomp/melee)
was read as a reference for physics *architecture*, and it's excellent work worth
knowing about. Notably it contains no frame data at all: character attributes live
in disc files it doesn't distribute, so all values here were derived by measurement
and tuning.

## License

MIT — see [LICENSE](LICENSE).
