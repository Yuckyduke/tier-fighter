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

Every divergence found between the Melee decompilation and this codebase. **None of
these block playtesting** — they're depth, not blockers.

Run `python3 tools/coverage.py` for live numbers. Currently **57%** of knowable
per-character attributes and **19%** of knowable global constants are covered. Those
percentages exclude the decomp's unnamed placeholder fields (409 of the 477 globals
are named `x1F0`, `xA4` and similar) — their purpose is unknown, so they can be
neither covered nor ruled out, and counting them would make the denominator fiction.

### Findings from the systematic passes

Audits over the decomp — movement, attacks, and the victim's side — turned up
mechanics the field-by-field coverage sweep could not see, because they live in
*control flow* rather than in named constants. Ordered by how much each would change
play.

**On method, for whoever does this next.** The movement and attack passes read broadly
and were expensive but found genuine surprises — teeter being selected by which
collision helper a state calls, for instance, which no constant-level sweep could
reveal. The victim-side pass was done differently: grep for structure first, then read
the *one* function that matters. That answered four specific questions in four commands
rather than fifteen minutes of reading. Read broadly to find unknown unknowns; grep
narrowly to answer questions you can already phrase.

#### Architectural — these are refactors, not additions

- [ ] **Multi-hit resolution is "best knockback wins", not first-hit-wins.** Hits are
      not applied on contact: they are logged, then a resolution pass recomputes
      knockback for every logged hit and applies only the **maximum**. Damage from all
      of them still accumulates. We `break` on the first overlap
      (`sim.cpp` already flags this as an MVP shortcut). Matters for any move with
      overlapping hitboxes, and for two players hitting each other on one frame.
- [ ] **Ground velocity as a scalar projected onto the floor normal.** They store 1D
      speed along the surface and *recompute* world velocity each frame from the floor
      normal. Every physics function writes an **acceleration** into an accumulator,
      integrated centrally once — so friction and acceleration share one channel and
      are mutually exclusive per frame. Ice scales your ability to *change* speed, not
      your speed. Prerequisite for slopes.
- [ ] **Hitboxes are swept capsules from frame 2, but a sphere on frame 1.** Getting
      this wrong in either direction is visible: sweep on frame 1 and you hit from
      behind, never sweep and fast hitboxes tunnel.
- [ ] **Teeter/edge-stop is chosen by which collision helper a state uses**, not by the
      state machine. Three modes: Wait/Walk/Landing teeter, shield-roll hard-stops,
      and **Dash/Run/Squat/Turn walk straight off**. That is why running off a ledge is
      instant while walking off makes you teeter.

#### Mechanics worth building

- [ ] **Stale-move negation** — and the subtle part: staling reduces **damage but not
      knockback**. Two separate fields on the hitbox; the unstaled value feeds the
      knockback formula while the staled one raises percent. So a worn-out move
      launches just as far but builds less damage. A 10-deep queue where only 9 entries
      are scanned, deduped on (move, instance) but looked up on move alone.
      "Refreshing" forces a new instance so the move stales *again* rather than
      clearing anything.
- [ ] **IASA (interruptible-as-soon-as)** — a single bit set by one animation opcode,
      not a computed window. Once open you get the entire neutral option list. Crucially
      the **combo-continuation checks sit outside that guard** and run every frame,
      which is why jab-cancels work when nothing else does.
- [ ] **Autocancel** — shares one function with L-cancel. Landing with the autocancel
      window unset gives plain `Landing` and *zero* aerial lag.
- [ ] **Crouch** — four states with hysteresis: a *smaller* threshold to exit than to
      enter, so it does not chatter. Gateway to crouch-cancelling.
- [ ] **Platform drop-through is buffered, not instant.** A down-flick arms a timer;
      only when it expires *and* you are still on a platform do you fall. Cancellable,
      and it costs your grounded jump. Shield-drop is a separate **instant** path with
      its own constants.
- [ ] **Phantom hits** — below a penetration-depth threshold a hit deals half damage,
      no knockback, and no hitlag. Tracked in a second victim list, so the same hitbox
      can still land a real hit later.
- [ ] **Clank is an asymmetric damage-difference test.** Two independent one-way
      comparisons against a tolerance, so **both can rebound, or neither**. A move
      sufficiently stronger passes through unclanked. Clanking consumes the swing.
- [ ] **Jab combo chaining** — an input buffer armed per jab. The press must land inside
      the window **and** the script must have opened a flag. An early press buffers and
      fires the moment it opens.
- [ ] **Rapid jab** re-arms its hitbox by bumping the attack instance on each animation
      loop wrap, which is also what makes it stale per cycle.
- [ ] **Hit-group** — hitboxes sharing a group share victim lists, which is how a
      sweetspot/sourspot pair on one swing hits once total.
- [ ] **Jump horizontal momentum is add-then-clamp**: carried velocity is scaled, the
      stick contribution is *added*, and the **sum** is clamped. So a dash-jump with
      neutral stick keeps more speed than one with forward stick.
- [ ] **Jump's first physics frame is skipped entirely** — no gravity, no drift, and you
      cannot act. Measurably changes jump height.
- [ ] **Walking off a ledge requires near-full stick tilt**; below the threshold you
      stop dead at the edge.
- [ ] **Turn flips facing, runs attack checks, then flips back** before jump/dash
      checks — so attacks out of a turn use the new direction but a dash uses the old
      one. That is what makes turnaround smashes work. It also buffers attack presses
      during the turn and replays them on completion.
- [ ] **Landing skips its lag below a velocity threshold** — and the threshold is
      negative, so the comparison is easy to invert.
- [ ] **Angled tilts and smashes** — five variants each, resolved once on the input
      frame from stick angle and baked into a motion state. Nothing about the angle is
      stored, which is why it survives charging trivially.
- [ ] **Tap jump sensitivity differs while running** — a separate threshold from
      standing.
- [ ] **Player push-apart is a position nudge**, recomputed from scratch each frame,
      never integrated into velocity. Being pushed backwards also switches you from
      teeter to hard edge-stop, so you cannot be shoved off a ledge into a teeter.
- [ ] **Wall bonk only above walk speed, and only from Dash/Run.** Ceiling bonk only
      from jump states, not from falling.
- [ ] **Edge slipping** while shielding is a distinct state from teeter, with inverted
      facing/flag pairing.
- [ ] **Flick timers must be explicitly consumed** by whoever uses them, or one flick
      triggers two mechanics. We do this for fast-fall and dash; it is needed wherever
      a flick is read.
- [ ] **Combo counter** keyed on (victim, attack id): same id increments, a different
      one resets.

#### Getting hit — the victim's side

A narrow follow-up pass. Nothing here contradicts what we have built; these are all
*additions*, which matches the victim side being our most complete area already.

- [ ] **Launch tiers.** Knockback is scaled then banded into four tiers, and the tier
      indexes a table of reaction states — **indexed by which hurtbox was hit**, so
      being struck in the head versus the legs picks a different reaction at the same
      tier. Tier 3 is tumble. A forced state (down-throw) overrides straight to tier 3.
      At tier 2+ the launch angle is recomputed if the element is Ice, and airborne
      victims get an extra knockback scale.
- [ ] **Armor.** Subtracted from knockback *after* the multipliers, in a fixed order:
      × crouch → × ice → × smash-charge → − armor → clamp to a floor. Two armor sources
      with `max()` between them, plus a metal-specific addition. The floor means armor
      can never fully negate a hit.
- [ ] **Crouch-cancelling is a knockback multiplier** applied before armor —
      mechanically simple, and it confirms the entry already on this list.
- [ ] **Intangibility is tri-valued**, not a boolean: Enabled / Disabled / Intangible
      per hurtbox. *Disabled* still registers contact (for the sound) but deals no
      damage; *Intangible* skips the test entirely. Our single `invulnFrames` countdown
      collapses all three into one.
- [ ] **The flying-body hitbox.** A launched player damages others, but only while
      their knockback speed is above a threshold. Keyed on an owner pointer so kills
      are credited to whoever launched them.

#### Known bugs in the original — deliberately NOT porting

Recording these so nobody "fixes" our version into matching them later.

- **Shield-knockback decay zeroes the wrong vector component**, clobbering the regular
  knockback vector. The decomp flags it as the cause of a known invisible-ceiling
  glitch.
- **The hurtbox loop breaks after the first overlap**, so if the highest-priority
  hurtbox yields a phantom, lower-priority ones that would have given a real hit are
  never tested. The decomp comments the intended fix.
- **The angled-attack availability check probes the neighbouring motion state's
  animation data**, not the one being selected.
- **Dead code:** a vertical velocity multiply in the jump path is overwritten three
  lines later. Faithfully reproducing it would be wrong.

### Mechanics — worth building

Each of these changes how the game plays.

- [ ] **Crouch + crouch-cancelling** — `kb_squat_mul` reduces knockback taken while
      crouching. A real defensive mechanic; I initially mis-filed crouch as cosmetic.
- [ ] **Tap jump** — `tap_jump_threshold`, `tap_jump_release_threshold`. Flick the
      stick up to jump. No such input exists here.
- [ ] **Multi-hit attacks** — needs a per-attack hit list. The current
      `attackConnected` bool allows exactly one hit per swing.
- [ ] **Attack clank** — `ReboundStop`/`Rebound`. Two attacks colliding cancel each
      other. Removes a whole layer of trades.
- [ ] **Rapid jab / jab combos** — `jab_2_input_window`, `jab_3_input_window`,
      `rapid_jab_window`. We have a single jab with no follow-ups.
- [ ] **Angled tilts** — their side tilt has five aim variants
      (`AttackS3Hi/HiS/S/LwS/Lw`); we have one.
- [ ] **Jump horizontal momentum** — 4 attributes including
      `ground_to_air_jump_momentum_multiplier`, which is how they tune whether
      dashing then jumping preserves your speed. We carry velocity through unchanged.
- [ ] **`hit_weight_mul`** — an extra weight term in the knockback formula.
- [ ] **Walk speed tiers** — `slow_walk_max`, `mid_walk_point`, `fast_walk_min`. We
      read stick magnitude directly, which is arguably better, but their tiers also
      gate animation blending.

### Blocked on stage geometry

Can't be built until stages are more than one flat platform.

- [ ] **Wall / ceiling collision** — `StopWall`, `StopCeil`, `FlyReflectWall`,
      `FlyReflectCeil`
- [ ] **Wall / ceiling tech** — `PassiveWall`, `PassiveWallJump`, `PassiveCeil`,
      plus `passivewall_vel_x`, `passiveceil_vel_x`, `passive_wall_vel_y_base`
- [ ] **Platform pass-through** (`Pass`) — needs soft platforms
- [ ] **`gr_vel` + slope projection** — they keep ground velocity as a *scalar* and
      project it onto the floor normal each frame. Prerequisite for sloped stages,
      and also how surface-specific friction (ice as a floor flag) would work.
- [ ] **Ledge teeter** (`Ottotto`), **edge slip** (`MissFoot`) — minor polish

### Structural refinements

Approximations that work but diverge from theirs.

- [ ] **ECB diamond instead of AABB** — their body is four points forming a diamond,
      swept from previous to desired position. We use a swept AABB: same
      anti-tunnelling property, simpler math. Affects ledge-grab and wall feel.
- [ ] **Per-bone hurtbox states** — their invulnerability is a hurtbox flag set by
      animation timeline events, per-bone. Ours is a single countdown per player. The
      finer resolution would let a roll be invulnerable in the torso while a trailing
      leg is still hittable.
- [ ] **Animation-driven roll travel** — they overwrite velocity from the animation's
      root-bone delta each frame. We interpolate a fixed distance, which produces the
      same "always covers the same ground" result without a rig.
- [ ] **Powershield** — `GuardReflect` plus its own timer web and reflect hitboxes.
      Deliberately deferred, not missed.

### Deliberately not porting

- **The airborne attacker-pushback decay.** The decomp flags this one itself: it
  zeroes the wrong vector component and clobbers the regular knockback vector,
  causing a known invisible-ceiling glitch. Faithfully reproducing a bug is not
  fidelity.

### Out of scope entirely

These account for most of the remaining "uncovered" count, and none will ever be
built here. Listed so the coverage percentage isn't mistaken for a to-do list.

| Category | Fields | Why never |
|---|---|---|
| Items and powerups | ~105 states + attrs | No items in this game |
| Character-specific grabs, bosses | ~64 | No Bowser, Yoshi, Kirby, Master Hand |
| Status effects | ~13 | Freeze, bury, sing, disable, metal — all character moves |
| Cosmetic | ~21 | Model scale, name tag height, trophy scale, camera angles |
| Presentation | ~5 | Match intro, taunts |

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
