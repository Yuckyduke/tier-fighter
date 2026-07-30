# State mapping: Melee → tier-fighter

A state-by-state pass over Melee's action-state enum (`ftCo_MotionState` in
`chara/ftCommon/forward.h`), classifying every entry against what we have.

**341 real states** (343 minus the `None` and `Count` sentinels). Every one is
accounted for below — nothing was skipped or estimated.

| Verdict | Count | Meaning |
|---|---|---|
| **HAVE** | 3 | Direct 1:1 match already implemented |
| **COLLAPSE** | 109 | Many of their states → one of ours. Already covered. |
| **LATER** | 55 | Real mechanics we don't have. Worth building. |
| **SKIP** | 174 | Items, character-specific grabs, status effects, presentation. Not needed. |

**Update:** ledges are now implemented — the 12 `Cliff*` states moved from LATER to
COLLAPSE (they map to 5 states here, since slow/quick is a parameter).

## Why 1:1 is the wrong target

**Over half their states (174) are content, not mechanics.** Six items × four
swing types is 24 states expressing *one* idea. Eleven `Dead*` states differ only
by death direction and camera treatment. Every grabbing character has its own
`Capture*`/`Thrown*` trio — Bowser, Yoshi, Kirby, Mewtwo, Master Hand, Crazy Hand,
Likelike. We have no items, no bosses, and no character-specific grabs, so those
names would be 174 empty enum values with no gameplay attached.

**Another 97 are variants that one parameterized state covers.** Their 18 `Damage*`
states (`DamageHi1/2/3`, `DamageN1/2/3`, `DamageFlyTop`, `DamageFlyRoll`, …) encode
launch angle and severity *in the state itself*. We carry a knockback vector, so
one `Hitstun` state expresses all of them — and expresses angles between theirs,
which a fixed set can't.

That's the guiding principle throughout: **match their structure where it's
load-bearing, collapse where it's cosmetic.**

---

## HAVE — direct matches (3)

| Melee | Ours |
|---|---|
| `Wait` | `Idle` |
| `Dash` | `Dash` |
| `KneeBend` | `Jumpsquat` |

`KneeBend` is worth noting: it's the pre-jump crouch, and its length being the
short-hop window is the mechanic, not the animation.

---

## COLLAPSE — covered by a parameterized state (97)

| Their states | → Ours | n |
|---|---|---|
| `DamageHi1-3`, `DamageN1-3`, `DamageLw1-3`, `DamageAir1-3`, `DamageFly{Hi,N,Lw,Top,Roll}`, `DamageFall` | `Hitstun` + knockback vector | 18 |
| `Fall`, `FallF/B`, `FallAerial{,F,B}`, `FallSpecial{,F,B}` | `Airborne` | 9 |
| `DeadDown/Left/Right/Up`, `DeadUpStar`, `DeadUpFall*` | `Dead` | 8 |
| `Landing`, `LandingFallSpecial`, `LandingAir{N,F,B,Hi,Lw}` | `Landing` | 7 |
| `AttackS3{Hi,HiS,S,LwS,Lw}`, `AttackHi3`, `AttackLw3` | `AttackGround` + `ATK_TILT_*` | 7 |
| `AttackS4{Hi,HiS,S,LwS,Lw}`, `AttackHi4`, `AttackLw4` | `AttackGround` + `ATK_SMASH_*` | 7 |
| `Attack11/12/13`, `Attack100Start/Loop/End` | `AttackGround` + `ATK_JAB` | 6 |
| `DownFoward{U,D}`, `DownBack{U,D}`, `DownSpot{U,D}` | `GetUpRoll` | 6 |
| `AttackAir{N,F,B,Hi,Lw}` | `AttackAir` + `ATK_AIR_*` | 5 |
| `JumpF/B`, `JumpAerialF/B` | `Airborne` (+ `airJumps`) | 4 |
| `WalkSlow/Middle/Fast` | `Walk` (analog stick magnitude) | 3 |
| `Passive`, `PassiveStandF/B` | `Tech` | 3 |
| `Rebirth`, `RebirthWait` | `Dead` (respawn timer) | 2 |
| `Run`, `RunDirect` | `Dash` | 2 |
| `DownBound{U,D}` | `Bounce` | 2 |
| `DownWait{U,D}` | `DownWait` | 2 |
| `DownStand{U,D}` | `GetUp` | 2 |
| `DownAttack{U,D}` | `GetUpAttack` | 2 |
| `DownDamage{U,D}` | `Hitstun` | 2 |
| `CliffCatch`, `CliffWait` | `LedgeHang` | 2 |
| `CliffClimb{Slow,Quick}` | `LedgeClimb` (frames latched from damage) | 2 |
| `CliffEscape{Slow,Quick}` | `LedgeRoll` | 2 |
| `CliffAttack{Slow,Quick}` | `LedgeAttack` + `ATK_LEDGE` | 2 |
| `CliffJump{Slow1,Slow2,Quick1,Quick2}` | `LedgeJump` | 4 |

### Notes on specific collapses

**The `U`/`D` suffix everywhere in the `Down*` family** is face-up vs face-down.
Melee decides it by reading a hip-bone transform (`ftCo_80097570` inspects
`FtPart_HipN`'s matrix). That's animation-driven and not reproducible without
skeletal data — so we have one orientation. Cosmetic unless you later add
orientation-dependent hitboxes.

**`AttackS3Hi/HiS/S/LwS/Lw`** are five *angled* variants of one side tilt — Melee
lets you aim it up or down. We have one. Worth revisiting as a directional
refinement, not a missing mechanic.

**`WalkSlow/Middle/Fast`** are discrete speed tiers. We read stick magnitude
directly, which is strictly more expressive.

**`Attack100*`** is the rapid-jab (mash A for a flurry). We have single jab only —
a small gap, listed under LATER-adjacent if you want the flurry.

---

## LATER — real mechanics we're missing (67)

Ordered by what I'd build first.

### 1. Shield + grab — 35 states (must land together)
- **Shield (5):** `GuardOn`, `Guard`, `GuardOff`, `GuardSetOff`, `GuardReflect`
- **Shield break (6):** `ShieldBreak{Fly,Fall,DownU,DownD,StandU,StandD}`
- **Roll / spot-dodge (4):** `Escape{F,B,N,Air}`
- **Grab (7):** `Catch`, `CatchPull`, `CatchDash`, `CatchDashPull`, `CatchWait`,
  `CatchAttack`, `CatchCut`
- **Throws (13):** `Throw{F,B,Hi,Lw}`, `Thrown{F,B,Hi,Lw}`, `ThrownF{F,B,Hi,Lw}`,
  `ThrownlwWomen`

**These are one feature, not two.** Shield with no grab makes shielding strictly
dominant — there'd be no answer to it. Grab with no shield makes grab useless,
since its whole purpose is punishing shields. The rock-paper-scissors of
attack/shield/grab is what makes a neutral game work, so all three must exist
before any of them is balanced.

Note `EscapeAir` is the air dodge — which we already have. So roll and spot-dodge
are the missing two-thirds of that family.

### 2. Smaller mechanics — 20 states

| Mechanic | States | Why it matters |
|---|---|---|
| Wall/ceiling collision | `StopWall`, `StopCeil`, `FlyReflectWall`, `FlyReflectCeil` | Stages with walls need this; ours is one flat platform |
| Turnaround commitment | `Turn`, `TurnRun`, `RunBrake` | We snap direction instantly. Real turnaround frames are what give dash-dancing its texture |
| Crouch | `Squat`, `SquatWait`, `SquatRv` | Also the gateway to crouch-cancelling |
| Wall/ceiling tech | `PassiveWall`, `PassiveWallJump`, `PassiveCeil` | Extends the tech system we just built to other surfaces |
| Ledge teeter | `Ottotto`, `OttottoWait` | The wobble at a ledge edge. Mostly cosmetic, but it telegraphs position |
| Attack clank | `ReboundStop`, `Rebound` | Two attacks colliding cancel each other. Removes a whole layer of trades |
| Platform pass-through | `Pass` | Needs soft platforms first |
| Slipping off edge | `MissFoot` | Minor |
| Dash attack | `AttackDash` | One move; trivial to add to the attack table |

---

## SKIP — not needed (174)

### Items and powerups — 105
Six weapon types × four swing types (`SwordSwing1/3/4/Dash`, then Bat, Parasol,
Harisen, StarRod, Lipstick) = 24. Plus `LightThrow*`/`HeavyThrow*` in ground and
air variants (26), the Super Scope's 16 charge/fire/empty states, `LGunShoot*`,
`FireFlower*`, `ItemScrew*`, `ItemParasol*`, `Lift*` (carrying heavy items),
`Barrel`, `WarpStar*`, `Hammer*` (7 states — the hammer replaces your whole
moveset), and `Kinoko*` (8 — mushroom grow/shrink, ground and air).

No items in the game. If items are ever added, the swing states are one mechanic
(item has jab/tilt/smash/dash) parameterized by item type — about four states, not
24.

### Character-specific grabs, bosses, status effects — 64
Every grabbing character has its own capture trio: `CaptureKoopa`/`ThrownKoopa*`
(Bowser), `CaptureYoshi`/`YoshiEgg`, `CaptureKirby`/`ThrownKirbyStar`,
`CaptureMewtwo`, `CaptureCaptain`, plus stage hazards (`CaptureLeadead`,
`CaptureLikelike`) and bosses (`CaptureMasterHand`, `CaptureCrazyHand`).

Status effects: `Bury*` (buried in ground), `DamageSong*` (Jigglypuff's sing),
`DamageBind` (Mewtwo's disable), `DamageIce*` and `DeadUpStarIce` (frozen),
`Sleep`, `Furafura` (dizzy), `Shouldered*` (being carried), `DownReflect`.

All of these presuppose specific characters or moves we don't have.

### Presentation — 5
`Entry`, `EntryStart`, `EntryEnd` (match intro), `AppealSR`, `AppealSL` (taunts).

---

## Summary

**Coverage of mechanics that apply to our game:**

```
Applicable states (341 - 174 skipped) = 167
  Covered (HAVE + COLLAPSE)  = 112   (67%)
  Missing (LATER)            =  55   (33%)
```

Of the 55 remaining, **35 are one feature**: the shield/grab/throw cluster. Build
that and coverage reaches ~88% of applicable mechanics.

**Recommended order:**
1. ~~Ledges~~ — **DONE**. 12 states → 5, slow/quick as a damage-driven parameter.
2. **Shield + roll + grab + throws together** — one atomic feature; splitting it
   inverts the balance
3. **Turnaround, crouch, dash attack, clank** — cheap polish once the above exist
4. **Wall/ceiling** — only once stages have walls

Everything in COLLAPSE and SKIP needs no further work. The 174 skipped states are
not deferred work; they're states that would be meaningless in this game.
