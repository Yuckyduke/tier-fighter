# Poses and the pose editor

The fighters are stick figures drawn procedurally from **pose data** — joint angles —
rather than from sprite images. This document covers why, how the data is shaped,
and how to author it.

## Why poses instead of sprites

A fighting game character needs a distinct look for every action state, and this one
has 42 of them: idle, walk, dash, run, jumpsquat, airborne, landing, thirteen
attacks, hitstun, knockdown, three get-up options, five ledge states, four shield
states, dizzy, two rolls, spotdodge, six grab and throw states, and death.

Hand-drawn, that's 40+ animations per character at several frames each — the single
largest time sink in a project like this, and the part engineering skill can't speed
up. Procedural stick figures sidestep it:

- **One pose is eleven numbers on one line.** All 42 states fit on roughly a page.
- **Two keyframes per state, not N frames.** The renderer interpolates between them.
- **Frame-accurate for free.** Animations are driven by the same `stateFrame` the
  simulation uses, so you can *see* attack startup versus active versus recovery —
  genuinely useful for a fighting game, and something sprite sheets have to be
  painstakingly synced to achieve.
- **Retuning a frame count retunes the animation.** Durations come from `config.h`,
  so shortening jumpsquat shortens its animation automatically.

The trade-off is honest: stick figures are a legibility solution, not an art style.
If real art arrives later, this gets replaced. It's worth doing now because with 42
states, rectangles no longer communicate what's happening — you can't tell a roll
from a spotdodge at a glance.

## The data

```
src/render/skeleton.h      Pose and Anim types, the public interface
src/render/skeleton.cpp    THE POSE TABLE -- all 42 states
src/render/stickfigure.cpp Draws a pose; proportions live here
src/render/pose_editor.cpp The editor
```

A pose is written with a shorthand so it reads as a single line:

```
P(lean, headY, shoulderF, elbowF, shoulderB, elbowB, hipF, kneeF, hipB, kneeB, crouch)
```

- **Angles are degrees from straight down**, positive clockwise for a figure facing
  right. Poses are authored right-facing only; the renderer mirrors for left.
- **`lean`** tilts the torso from vertical.
- **`crouch`** compresses the whole figure toward the feet, 0 to 1.
- **`headY`** raises or lowers the head as a fraction of height, for tucking.
- **Front and back limbs** are drawn separately, with the back pair dimmed, so the
  figure reads three-dimensionally instead of as a tangle of identical lines.
- **Lengths are fractions of body height**, so one pose fits any character size.

Each state gets one of three animation forms:

| Form | Behaviour |
|---|---|
| `Hold(a)` | A single static pose |
| `Lerp(a, b)` | Interpolates a → b across the state's natural duration |
| `Cycle(a, b, n)` | Ping-pongs a ↔ b on an `n`-frame cycle (walk, run, idle sway) |

`Lerp` reads the state's duration from `config.h` via `naturalDuration()`, which is
why animations stay in sync with gameplay timing automatically.

## The editor

```sh
./build/tf_poses
```

Deliberately separate from the game — no opponent, no physics, no arena. One figure,
one state, large and centred, so the only question on screen is whether the pose
reads correctly.

| Key | Action |
|---|---|
| `[` `]` | Previous / next state |
| `↑` `↓` | Select joint |
| `←` `→` | Adjust it (hold `Shift` for ×4) |
| `Tab` | Switch between keyframe A and B |
| `Space` | Play / pause |
| `,` `.` | Step one frame |
| `R` | Restart the animation |
| `D` | **Dump the pose to stdout, paste-ready** |
| `G` | Toggle the height grid |
| `S` | Toggle the keyframe previews |
| `+` `-` | Zoom |

On screen: the animated figure centre stage, keyframes A and B dimmed either side so
you can see where the motion starts and ends without scrubbing, and a small mirrored
copy to confirm facing-left reads correctly.

## Workflow

1. `./build/tf_poses`
2. Page through with `]` until something reads badly
3. `Tab` to the keyframe that's wrong, `↑↓` to the joint, `←→` to adjust
4. `Space` to watch it in motion, `,` `.` to scrub individual frames
5. `D` to print the pose
6. Paste it into the table in `skeleton.cpp`

Step 5 is what makes iteration fast — the dump is in exactly the `P(...)` form the
table uses, so there's no guessing which number produced what you saw.

The table is shared between the editor and the game, so a pose tuned here improves
`tf_play` on the next build. They cannot disagree about what a state looks like.

## Current status

First-pass poses exist for all 42 states, written from reasoning about what each
state should communicate rather than from observation. **Expect some to read badly** —
that's what the editor is for.

The game client still draws rectangles; the swap to stick figures is pending, and
will keep a toggle back to boxes for hitbox debugging.

## Adding a state

If a new `ActionState` is added to the simulation:

1. Add an entry to `kEntries` in `skeleton.cpp`
2. Add its animation to `kAnims`, in the same position

A `static_assert` enforces that the two arrays stay the same length, so a missing
pose fails the build rather than silently drawing the wrong thing.
