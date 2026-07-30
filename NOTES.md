# tier-fighter — design notes

A Melee-inspired platform fighter. Flat pool of stages, scored by KOs, endless.

## The game loop (MVP)

- Each **stage** is a persistent room running an independent match. Stages are
  created on demand and closed when empty, so **stage count tracks population** —
  nothing is pre-allocated.
- **Score a KO** → points, streak increment, and a **full heal back to 0%**.
- **Get knocked off** → ejected and routed to a **different** stage. Streak
  resets. No elimination, no waiting; you're in a new fight immediately.
- Streak bonus pays at *every* multiple of the threshold (5), so a 15-KO run earns
  it three times.

### Why the full heal is the central mechanic

It makes holding a stage *possible but never safe*. A strong player can survive a
stream of arrivals — but only while they keep converting. Take damage without
scoring and you become as launchable as anyone else. It's a self-balancing pressure
valve: dominance is rewarded, camping isn't.

### Why there are no levels/tiers

Tried and removed. A tiered tower needs a large concurrent population for its upper
floors to be populated at all — at 20 players online, whoever climbs highest sits
alone with nobody to fight. A flat pool behaves identically at 4 players or 400.
Tiers can layer on later once population justifies them.

### Routing rules (these produce the chaos)

1. An ejected player **never returns to the stage they just left**, unless it's
   genuinely the only option. Fresh opponents every time.
2. Arrivals fill the **fullest** non-full stage. Packing rather than spreading is
   deliberate — emptiest-first would balance populations neatly and produce lots of
   quiet duels, which is the wrong feel for a game about being swarmed.

## What was learned from reading the decomp

The doldecomp/melee project was read as **reference only** (cloned to `/tmp`, never
a dependency). Three findings changed the code:

### 1. Velocity is decomposed, not a single vector

`Fighter` in the decomp carries `self_vel` (your own movement) and a separate
`kb_vel` (knockback), which sum to produce motion.

This is load-bearing. It's why you can air-drift *while* being launched: drift
accumulates in `selfVel` while `kbVel` decays independently on its own schedule.
Collapse them into one `vx/vy` and that interaction disappears — combos and
recovery both stop feeling right, and no amount of constant-tuning brings them
back. `Player` in `state.h` mirrors the split. Test: `testAirDriftDuringHitstun`.

### 2. Collision is swept, and the body is a diamond

The ECB (`ftECB`) is four points — top/bottom/left/right as `Vec2` — forming a
diamond, not an AABB. `CollData` keeps `prev_ecb` and `desired_ecb` alongside the
current one, so collision resolves against the *movement segment* rather than the
final position. That's what stops fast-falling players tunneling through platforms.

We implement a **swept AABB**: same anti-tunneling property, simpler math. The
diamond is a later refinement affecting ledge-grab and wall-interaction feel.

### 3. Melee uses floats freely — we can't

The decomp's physics calls `atan2f`, `sqrtf`, and uses `float` throughout. Fine for
Melee: it was local-only, so cross-machine determinism was never a requirement.

For an online game it's the whole ballgame. Hence fixed-point Q16.16 everywhere
(`fixed.h`), a baked sine table (`trig.cpp`), and an integer `fx_sqrt`.
`-ffast-math` is explicitly disabled in CMake and must never creep back in.

### What the decomp does NOT contain

**The physics values aren't there.** It decompiles `main.dol` — executable code
only. Character attributes (gravity, walk speed, weight, jumpsquat frames) and all
animation/hitbox frame data live in the disc's per-character `Pl**.dat` files, which
the project doesn't distribute and doesn't need. Every constant in `config.h` is
ours, and tuning them is the game design work.

The README also notes the result "won't be portable (aka you can't compile it to run
on a normal computer)" — confirming it could never have been a code foundation.

## The central design lesson: write rules, not techniques

Nobody at HAL wrote a `Wavedash()` function. Wavedashing, L-cancelling, and
dash-dancing are **emergent** — consequences of simple orthogonal rules interacting.

Wavedash falls out of exactly three independent rules:
1. an air dodge sets velocity along the stick
2. landing zeroes only **vertical** velocity
3. ground friction merely *decays* horizontal velocity

The technique lives in the gap between them. `grep -i wavedash src/` returns only
comments — there is no wavedash code, yet `testWavedashEmerges` proves it works.
That test was verified by deliberately breaking rule 2 (zeroing horizontal velocity
on landing); the wavedash died immediately, confirming the test is load-bearing
rather than passing trivially.

**Implication:** code a "wavedash feature" and you get one scripted-feeling
technique. Code the momentum rules right and players discover things you never
designed — the biggest single reason Melee still has a scene 24 years later.

## Architecture

```
src/sim/
  fixed.h     Q16.16 math. No float constructor exists, so floats can't leak in.
  trig.h/cpp  Baked sine table — std::sin isn't bit-identical across platforms.
  input.h     One frame of input. Tiny POD: the network + rollback unit.
  config.h    ALL tunables, grouped by concern. The only place numbers live.
  state.h     Player + GameState. Trivially copyable (asserted in tests).
  sim.cpp     step(state, inputs) -> state. Pure. The whole simulation.
  arena.h/cpp Stage pool, KO scoring, streaks, routing, leaderboard.
```

**The invariant everything depends on:** `step()` is a pure function. No I/O, no
clock reads, no allocation, no floats, no randomness. Same inputs always produce
bit-identical output on any machine. Rollback netcode is only possible if this
holds, which is why `testDeterminism` asserts snapshot → restore → re-simulate
reproduces state exactly. **That test is rollback, minus the network.**

The sim/arena split matters too: the simulation knows nothing about scoring or
matchmaking. It reports KOs via `Player::pendingKOs` and stock-outs via
`active = false`; the arena interprets them. Either can be rewritten without
touching the other.

## Status

**620 checks passing** (530 sim + 90 arena). Built and run, not asserted from
inspection.

### Mechanics implemented since the first pass

- **Full attack matrix** — jab, 3 tilts, 3 smashes (chargeable), 5 aerials, get-up
  and ledge attacks. Smash-vs-tilt is distinguished by stick-flick RECENCY, one
  timer, exactly as the decomp's `checkLStick` does it — so flick+attack gives a
  smash and hold-then-attack gives a tilt through a single code path.
- **Per-character everything** — `Player::charId` indexes `kFighters[]`; gravity,
  weight, jump, attack tables, knockdown and ledge values are all per-fighter.
  Two characters exist (Scout, Bruiser) to keep that plumbing honest.
- **Knockdown / tech** — hitstun landings branch on the knockback SURVIVING at
  impact (bounce / knockdown / normal landing), with tech, tech lockout, and
  get-up options. See `STATES.md`.
- **Ledges** — grab (requires FACING the ledge), hang with a timeout, climb / roll /
  attack / jump / drop, and a regrab cooldown on every exit. Slow-vs-quick options
  are chosen by damage.
- **Hitlag** — both fighters freeze on contact, scaled by damage. Knockback is
  assigned at contact but nothing moves until the freeze expires, so a hit reads as
  an impact rather than a teleport. This is the window SDI will use.
- **Per-aerial landing lag, flick-gated fast-fall, proportional air drift, a hard
  horizontal ceiling separate from the drift target, and turnaround acceleration
  exemption** — see `PHYSICS.md` for how each diverged before.

- **Shield** — health with three flows (drains held, regenerates when not,
  proportional + flat damage on hit), shieldstun from the LARGEST hit while health
  loss uses the SUM, both players pushed apart, break → dizzy whose length shrinks
  with damage. Out of shield: roll or spotdodge, with windowed invulnerability.

- **Grab and throws** — grab is a hitbox with a catch element; the hold timer
  scales with the victim's damage; escape is by mashing with two independent
  drains per frame; four throws chosen by a rising-edge flick, guaranteed once
  started. Throw knockback uses a global weight constant, so victim weight scales
  throw *duration* rather than distance.

**Not built yet:** netcode, matchmaking transport. The sim
library deliberately has zero dependencies so it compiles into a headless server
unchanged.

## Known gaps / next decisions

- **Nothing is visible.** Movement is proven by tests, not by feel — and feel is
  what actually needs judging. Every constant in `config.h` is my guess.
- **ECB is an AABB, not a diamond.** Affects ledge-grab feel specifically.
- **One hitbox per attack, single-target.** Multi-hit moves need a per-attack hit
  list rather than the current `attackConnected` bool.
- **No ledges, platforms, or shielding.** Stage is one flat platform.
- **Two attacks only** (jab, aerial) and one character.
- **KO credit is last-hitter-wins.** No assist credit or damage-share.
- **Battle royale mode** (mentioned as a later idea): the arena already supports it
  cheaply — pin stage count, switch ejection to elimination, shrink population.
  The elimination path is deliberately kept clean for this.
