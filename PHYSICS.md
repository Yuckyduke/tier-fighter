# Physics audit: Melee vs tier-fighter

A pass through Melee's movement code (`ft/ftcommon.c`, `ft/fighter.c`,
`ft/ft_081B.c`, `ft/inlines.h`, and the `ftCo_DatAttrs` struct in `ft/types.h`),
noting where we match and where we diverge.

`ftCo_DatAttrs` is effectively their physics checklist — every per-character
movement value in one struct. Walking it field by field is the cleanest way to see
what we're missing.

---

## The one function that defines their movement

`ftCommon_8007D174(fp, vel, accel, target_vel, friction)` is the heart of it. Every
movement path funnels through it. Paraphrasing the logic:

```
if target_vel == 0            -> apply friction, done
if sign(vel) != sign(accel)   -> use accel as-is (turning around: no cap)
else if accelerating past target:
      accel = -friction               (decelerate instead of overshoot)
      if that undershoots target      -> accel = exactly (target - vel)
      if that exceeds the hard cap    -> accel = exactly (cap - vel)
```

Three things worth extracting:

**1. Acceleration is never applied blindly.** It's clamped so velocity lands
*exactly* on the target rather than oscillating around it. Our `fx_min`/`fx_max`
approach gets the same result, so this one we match in effect.

**2. Turning around is exempt from the cap.** When `vel * accel < 0` — you're
pushing against your current motion — the acceleration is used unclamped. That's why
reversing direction in the air feels responsive rather than mushy. **We do not have
this exemption**, so our turnarounds are slower than theirs.

**3. There's a separate hard ceiling.** `air_max_horizontal_velocity` is distinct
from the drift target `air_drift_max`. Momentum from a launch or a wavedash can
exceed the drift target but never the ceiling. **We have one value**, so we can't
express "you can be moving faster than you could accelerate to."

---

## Field-by-field against `ftCo_DatAttrs`

### Walking — we diverge

| Theirs | Ours |
|---|---|
| `walk_init_vel` | — |
| `walk_accel` | — |
| `walk_max_vel` | `walkSpeed` |
| `slow_walk_max`, `mid_walk_point`, `fast_walk_min` | — (stick magnitude read directly) |

Their walk has an initial velocity, an acceleration, and a max — it *ramps*. Ours
assigns `walkSpeed` directly (`sim.cpp:269`), so walking is instant-on at full
speed. The three tier thresholds are for animation blending; reading stick magnitude
is arguably better, but the missing acceleration is a real difference in feel.

### Dashing and running — NOW IMPLEMENTED

`Dash`, `Run`, `RunBrake`, and `Turn` are distinct states. Dash is interruptible
(a fresh flick re-enters it — that's dash-dancing); Run is not, and leaving it
requires braking. Run acceleration tapers by `(1 - vel/target)` per
`ftCo_Run_Phys`. The dash impulse is velocity-relative per `ftCo_Dash_Enter`, so
reversals snap while continuing stays smooth.

Verified: a held dash commits to a run after `dashFrames`; a dash-dance holds
position with non-accumulating drift (21px at 5, 20, and 40 cycles); reversing out
of a run forces a brake.

### Original analysis — dashing and running

| Theirs | Ours |
|---|---|
| `dash_initial_velocity` | — |
| `dash_run_acceleration_a` | — |
| `dash_run_acceleration_b` | — |
| `dash_run_terminal_velocity` | `dashSpeed` |
| `max_run_brake_frames` | — |
| `frames_to_change_direction_on_standing_turn` | — |
| `ground_max_horizontal_velocity` | — |

**This is our largest movement gap.** They have four values describing a dash
(initial burst, two acceleration constants, terminal velocity); we have one number
assigned instantly. And critically they separate `Dash` from `Run` as distinct
states — `Dash` is interruptible, `Run` is not, and leaving `Run` requires
`RunBrake` over up to `max_run_brake_frames`.

That split is the mechanic, not a detail: **dash-dancing exists because `Dash` can be
cancelled and re-entered while `Run` cannot.** With one snap-to-speed state there is
nothing to dance with. `frames_to_change_direction_on_standing_turn` compounds it —
turning around is a timed commitment, where ours flips `facing` for free.

### Jumping — mostly match, two gaps

| Theirs | Ours |
|---|---|
| `jump_startup_time` | `jumpsquatFrames` ✓ |
| `jump_v_initial_velocity` | `fullVelocity` ✓ |
| `hop_v_initial_velocity` | `hopVelocity` ✓ |
| `air_jump_v_multiplier` | `airJumpVelocity` ✓ (absolute, not a multiplier) |
| `max_jumps` | `maxAirJumps` ✓ |
| `jump_h_initial_velocity` | — |
| `jump_h_max_velocity` | — |
| `ground_to_air_jump_momentum_multiplier` | — |
| `air_jump_h_multiplier` | — |

The gaps are all **horizontal** jump behavior. They give a jump its own horizontal
initial velocity and cap, and scale carried ground momentum into the air by
`ground_to_air_jump_momentum_multiplier`. We carry `selfVelX` through unchanged.
That multiplier is how they tune "does dashing then jumping preserve your speed" —
a knob we don't have.

### Air movement — partial match

| Theirs | Ours |
|---|---|
| `grav` | `gravity` ✓ |
| `terminal_vel` | `termVelocity` ✓ |
| `fast_fall_velocity` | `fastFallSpeed` ✓ |
| `aerial_friction` | `air.friction` ✓ |
| `air_drift_max` | `air.maxSpeed` ✓ |
| `air_drift_stick_mul` | — |
| `aerial_drift_base` | `air.acceleration` (partial) |
| `air_max_horizontal_velocity` | — |

Their air drift acceleration (`ftCommon_8007D28C`) is:

```
accel = (stick_x * air_drift_stick_mul) + (sign(stick_x) * aerial_drift_base)
```

A **flat term plus a stick-proportional term.** So a light stick tilt drifts slowly
and a full deflection drifts hard, but there's always a floor. We use a single
constant acceleration regardless of stick magnitude — so partial air drift is
impossible; ours is all-or-nothing past the deadzone.

### Landing lag — we diverge

| Theirs | Ours |
|---|---|
| `normal_landing_lag` | `landing.aerialLagFrames` |
| `landingairn_lag` | — |
| `landingairf_lag` | — |
| `landingairb_lag` | — |
| `landingairhi_lag` | — |
| `landingairlw_lag` | — |

**They have per-aerial landing lag — five separate values.** We have one shared
number, so every aerial is equally safe to land with. That flattens a real decision:
in Melee, choosing which aerial to throw out includes "how punished am I if I land
with it." Cheap to fix — it belongs in `AttackData` alongside the other frame data.

### Fast-fall — we diverge subtly, and this one is interesting

`ftCommon_CheckFallFast` requires **three** conditions:

```
not already fast-falling
AND self_vel.y < 0                      (descending)
AND stick.y past the threshold
AND timer_lstick_tilt_y < window        (the stick was just FLICKED down)
```

That fourth condition is the same flick-recency timer that distinguishes smashes
from tilts. **Fast-fall requires a fresh downward flick, not merely holding down.**
Ours checks only `in.stickY > kStick.hard`, so holding down while rising then
descending triggers it automatically.

We already have `stickHeldY` for the smash system, so this is a two-line fix.

Also note they set `timer_lstick_tilt_y = 0xFE` on success — deliberately
saturating it so the same flick cannot re-trigger.

### Weight and knockback — match

| Theirs | Ours |
|---|---|
| `weight` | `body.weight` ✓ |
| Knockback formula shape | ✓ (see `computeKnockback`) |
| `kb_vel` separate from `self_vel` | ✓ (`kbVelX/Y`) |

### Ledges — now match, one value missing

| Theirs | Ours |
|---|---|
| `ledge_jump_horizontal_velocity` | `ledge.jumpVelX` ✓ |
| `ledge_jump_vertical_velocity` | `ledge.jumpVelY` ✓ |
| Facing requirement | ✓ (`requireFacing`, added) |
| `ledge_cooldown` | `ledge.cooldownFrames` ✓ |
| `ledge_snap_height` (scales with character size) | — |

### Shield — absent

`initial_shield_size`, `shield_break_initial_velocity`, and the whole `Guard*`
family. Covered in `STATES.md`; not a physics gap so much as a missing feature.

### Wall/ceiling — absent

`passivewall_vel_x`, `wall_jump_horizontal_velocity`,
`wall_jump_vertical_velocity`, `passiveceil_vel_x`. Needs stages with walls first.

### Jab timing — absent

`jab_2_input_window`, `jab_3_input_window`, `rapid_jab_window`. We have a single jab
with no follow-ups.

---

## Structural divergences (bigger than any single value)

### 1. Ground velocity is a separate scalar, projected onto the slope

They keep `gr_vel` — a **scalar** along the ground — plus `xE4_ground_accel_1`, and
project both onto the floor normal each frame (`ftCommon_ApplyGroundMovement`):

```
self_vel.x = +normal.y * gr_vel
self_vel.y = -normal.x * gr_vel
```

So walking up a slope is walking along the surface, and the 2D velocity is derived.
We store `selfVelX/Y` directly and have a flat stage, so this hasn't mattered — but
**it's a prerequisite for sloped ground.** Worth knowing before adding slopes.

`ft_GetGroundFrictionMultiplier` also means friction is a property of the *surface*,
not just the character — ice is a floor flag, not a special case in the fighter.

### 2. Acceleration is stored, not applied

They write into `x74_anim_vel` / `xE4_ground_accel_1` and apply it later in the
frame. We mutate `selfVelX` immediately. Same outcome for us, but their split is
what lets one function serve both ground and air paths.

### 3. Hitlag — we don't have it at all

`ftCommon_CalcHitlag(dmg, msid, mul)` computes a freeze on hit:

```
frames = (dmg * c1 + c2) * mul
crouching (Squat) multiplies it further
```

Both fighters freeze for that many frames on contact. `fighter.c:1417` counts it
down and skips normal processing while it runs.

**We have no hitlag.** Hits resolve and knockback applies on the same frame. Two
consequences:
- **No impact weight.** Hitlag is most of what makes a heavy hit *feel* heavy —
  the momentary freeze reads as force.
- **No SDI.** `allow_sdi` is set during hitlag; smash-directional-influence is
  input during the freeze that shifts your position. It's a whole defensive
  mechanic that only exists because hitlag exists.

Notably `ft_80081F2C` disables ledge-snapping while `allow_sdi` is set, so hitlag
interacts with ledge grabs too.

This is the most significant physics omission we have — bigger than the dash gap,
because it affects how every hit in the game feels.

---

## Priority list

Ordered by impact per unit of work:

| # | Gap | Why | Status |
|---|---|---|---|
| 1 | **Hitlag** | Most of what makes hits feel weighty. Affects every exchange. | **DONE** |
| 2 | **Fast-fall needs a flick** | Was auto-triggering from held-down. | **DONE** |
| 3 | **Per-aerial landing lag** | Restores "which aerial is safe to land with". | **DONE** |
| 4 | **Air drift stick-proportional term** | Partial drift instead of all-or-nothing. | **DONE** |
| 5 | **Turnaround accel exemption** | Snappier reversals. | **DONE** |
| 6 | **Separate hard velocity ceiling** | Momentum can exceed the drift target. | **DONE** |
| 7 | **SDI** | Needs hitlag as its window. | **DONE** |
| 8 | **Dash / Run split + turn frames** | Unlocks dash-dancing. | **DONE** |
| 9 | **Walk/dash acceleration** | Movement was instant-on. | **DONE** |
| 10 | **Crouch + crouch-cancel** (`kb_squat_mul`) | Reduces knockback taken. Was miscalled cosmetic. | open |
| 11 | **Tap-jump** (`tap_jump_threshold`) | Flick up to jump. No such input exists. | open |
| 12 | **Jump horizontal momentum transfer** | Tunes dash-then-jump. | open |
| 13 | **`ground_max_horizontal_velocity`** | Ground speed ceiling. | **DONE** |
| 14 | **`gr_vel` + slope projection** | Only matters once stages have slopes. | open |

### SDI — implemented

`ftCo_Damage_OnEveryHitlag` is the whole mechanic, and five details of it matter:

1. It runs on **every** hitlag frame, not once per hit — a long freeze offers
   several nudges to anyone who can re-flick fast enough.
2. The magnitude gate is on the stick **vector** (a squared-length compare), not
   per-axis, so a diagonal qualifies even when neither axis alone clears the bar.
3. **Either** axis flicking fresh is enough — an `||`, not an `&&`.
4. It adds directly to **position**, not velocity. That is why SDI reads as an
   instant displacement rather than as drift: it bypasses velocity entirely.
5. Both stick timers **saturate** on success, so one flick buys exactly one nudge.
   Multi-SDI requires genuine re-flicking, which is what makes it a technique
   rather than a reward for holding a direction.

Verified: 4 nudges at 2-frame re-flicks, 3 at 3-frame, 2 at 4-frame, 0 with no
flick — and a stick held from before the hit earns nothing, with position frozen
through the entire freeze.

We add a per-freeze cap (`maxNudgesPerHitlag`) that they do not have, so a long
freeze cannot be ridden across the stage by mashing.

## The full gap list

Every remaining divergence is enumerated in **[PLAN.md](PLAN.md)**, grouped by
whether it is a mechanic worth building, blocked on stage geometry, a structural
refinement, deliberately not ported, or out of scope entirely. That grouping matters:
the raw coverage percentage counts items that will never be built here (items,
bosses, status effects), so reading it as a to-do list overstates the work.

## Measuring coverage

`tools/coverage.py` extracts every field from both attribute structs and diffs them
against `config.h`. It exists because the first hand audit was wrong twice: it
conflated "mentioned in prose" with "implemented", and an awk range silently
returned zero for `ftCommonData` (477 fields) which read as "nothing there" rather
than "broken query". Every extractor now asserts a plausible minimum and dies loudly.

Placeholder fields (`x1F0`, `xA4`) are excluded from percentages. 409 of the 477
global constants are unnamed in the decomp, so their purpose is unknowable -- they
can be neither covered nor ruled out, and counting them as gaps would make the
denominator fiction.

Per-character attrs went **28% -> 39%** in this pass. The tool's `EQUIVALENT` map is
hand-written and is the one non-mechanical part; each entry is a claim that could be
wrong. It also cannot see behavioural divergence -- it confirms a `gravity` field
exists, not that gravity behaves the same.
