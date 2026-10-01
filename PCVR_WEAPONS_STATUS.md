# Weapons build — where we are

Status against [`PCVR_WEAPONS_PLAN.md`](PCVR_WEAPONS_PLAN.md), part by part. Written so that
anybody — or any later session — can pick the work up without reconstructing it from commits.

Last updated 1 October 2026.

## IT RUNS

**1 October 2026: the weapon simulator worked in the game for the first time.**
Confirmed in a headset: the pistol slide racks, the revolver cylinder swings out
and back smoothly, the shotgun pump works, the MP5 fires and grips well.

Everything built before today was real code that had never once executed where a
player could feel it. Four separate faults stood between the simulator and the
hands, and none of them were in the simulator:

1. **Client prediction had never run.** A model index used as a string pointer,
   then a step of zero milliseconds, then a loopback that never negotiated the
   hand channel so the server never stepped either.
2. **EVERY VR BUTTON WAS OFF BY ONE.** `vr_openxr.h`'s VR_BTN_* constants index
   `vr.btn[]` directly and must equal `vr_action_id_t`. Inserting VRA_TRIGGER
   shifted the enum and the header was not renumbered, so VR_BTN_OFFGRIP read
   the MENU button - no weapon part in the game could be grabbed, by anyone,
   ever - and VR_BTN_RELOAD read USE, so the off-hand trigger dropped magazines
   on its own. Twelve STATIC_ASSERTs now fail the build if it recurs.
3. **The renderer read a different file from the game DLL.** The game DLL parses
   `vr/cards/*.card`; the renderer parses `models/vr/weapons.txt` to decide which
   bones are grabbable. That file did not exist, so the renderer fell back to a
   cvar naming one or two bones per weapon from before cards existed.
   `tools/vrcard/make_weapons_txt.py` generates it from the cards now, on every
   deploy.
4. **The hand was sent back under the wrong number.** Posing matched parts to
   joints by bone name; input did not, so on any weapon whose part order differs
   from its joint numbering the hand drove the wrong joint.

And one that hid all four: **the deploy tool copied `xash.dll` and the two game
DLLs and nothing else.** `ref_gl.dll`, `filesystem_stdio.dll`, `menu.dll`,
`vgui_support.dll` and the launcher were never deployed - the renderer in the
play install was missing the very commit that teaches it to read cards.

## The two documents

- [`PCVR_WEAPONS_PLAN.md`](PCVR_WEAPONS_PLAN.md) — *XashPCVR Weapons: The Full Build*, Parts A–O.
  This is the live plan. It replaced step 2 of the platform plan and Part E of it.
  **The plan has measured errors in it. See "Where the plan is wrong" below before building from it.**
- [`PCVR_PLATFORM_PLAN.md`](PCVR_PLATFORM_PLAN.md) — *Platform Direction and Architecture*, Parts
  A–K and a work order of steps 0–8. **On hold** since 29 September. Steps 0 and 1 are done; step
  2 is what the weapons plan replaced. Paused, not abandoned.

Also still current: [`PCVR_FEEL_CHECKLIST.md`](PCVR_FEEL_CHECKLIST.md), which is the contract any
rewrite must not silently break. **None of it has been verified in a headset.**

## The goal, as the owner states it

**Netplay safe. HL, OpFor and BShift at AAA quality, or as close as we can get. Mod-friendly in time.**
We ship the engine; the player brings their own Half-Life install. That last clause is what makes
retail content the only binding target — see the card section.

## Part by part

| Part | State | Where it lives |
| --- | --- | --- |
| **B** Architecture | done | the split holds; posing via `pfnGetJointValues`, game API v4 |
| **C** Mechanism simulator | done | `hlsdk/dlls/vr_joint.*`, `vr_trigger.*` |
| **D** Rounds and feed | done | `hlsdk/dlls/vr_feed.*` — magazine, tube, belt, cylinder, single-shot, thrown |
| **E** Cards and mesh surgery | **part** | parser and surgery work; **a card with a synth part cannot bind** |
| **F** Held weapon as an object | done, gated | `engine/client/vr/vr_hold.*`, behind `vr_hold_sim`, default 0 |
| **G** Hands on the gun | **mostly** | grip solver built from the authored fist; controls measured on the pistols, revolver, grenade and satchel |
| **H** Sights and scopes | not started | — |
| **I** World and body | not started | — |
| **J** Half-Life's arsenal | **part** | 18 HD cards written, 6 verified clean |
| **K** Opposing Force | done | 7 cards in `tools/vrcard/cards/gearbox/`, all valid against retail |
| **L** Malfunctions, fidelity | **done** (simulator side) | `hlsdk/dlls/vr_feed.*` - three levels, three jams, deterministic rolls |
| **M** Multiplayer | designed | split authority won a three-way design race; not built |
| **O** Testing | **part** | 117 headless cases, 9-build determinism; nothing needing a headset |

**The simulator now runs.** Until 1 October it had never executed inside Half-Life at all — not
"untested in a headset", never run. Client prediction dereferenced a model index as a pointer, then
stepped with `msec = 0`, and loopback never negotiated the hand channel so the server never stepped
either. Fixed in `b5b77137` and `989358e3`. **It still has not been run in a headset.**

## Blocking defects, in priority order

These are engine- or simulator-level and **no card can work around them**.

1. **A card declaring a synthetic part cannot bind.** `Mod_StudioFingerprint` (mod_studio.c:298) reads
   the cached model via `Mod_StudioExtradata`, and `Mod_LoadStudioModel` replaces that cache with the
   surgery's grown buffer during load. So the fingerprint is taken from the *post*-surgery model while
   the card declares what the generator measured from the file on disk. The surgery still runs,
   because it is driven by `VRGame_CardRaw`, which deliberately skips the fingerprint check — so the
   player gets a carved mesh with vanilla behaviour. **Fix: capture the fingerprint from the
   pre-surgery bytes in the surgery hook.** A fingerprint identifies the file the artist shipped;
   computing it after we have modified the model is the wrong moment.
2. **A joint cannot say it has no return spring.** `VRMachine_AddJoint` (vr_joint.cpp:121) gives every
   joint `VRJ_DEF_SPRING` regardless of type, and `VRGun_Init` (vr_gun.cpp:203) only overrides on
   non-zero, so zero means "keep the default". A revolver cylinder and a grenade's pin ring therefore
   spring home the moment the hand leaves — **a revolver reload is impossible**, because every
   `vr_feed` dump and load path requires `cyl_open`. The comment claims "the default for this kind of
   joint"; there is one value for all kinds. **Fix: per-type defaults, and make "no spring"
   expressible.**
3. **`fire semi auto` starts in SEMI.** `VRTrigger_Init` (vr_trigger.cpp:41) tests semi first, so a
   weapon declaring both positions draws single-shot. The MP5 is affected on both its cards.
4. **A card with no `type` clobbers `m_iClip` from −1 to 0** on every tick, which puts a spurious "0"
   on the hornet gun's HUD where vanilla draws none.
5. **A declared-but-undriven joint replaces its bone's animation** rather than falling through to it,
   so declaring a cosmetic joint is a net loss to the player.

## Where the plan is wrong

Measured, not inferred. 65 corrections were found in one pass; these are the ones that would cost a
wrong implementation.

- **The hand model the plan and this file pointed at is not the one the engine loads.**
  `vr_hand_hevsuit.mdl` (26 bones, 0 sequences) is real and measures as described, but nothing loads
  it. `vr_openxr.c:1882-1883` loads **`v_hand_hevsuit.mdl`** and `v_hand_labcoat.mdl`: 31 bones,
  9 sequences, 15 finger bones, **no metacarpal bones**, Bip01 naming. The GL renderer also cannot
  draw a `numseq == 0` model at all, so targeting the 26-bone rig costs an engine change as well.
- **The loaded hand ships an authored closed fist, and nothing mentioned it.** `fullgrab_start`, 21
  frames: MCP +68.1°, PIP +82.2°, DIP +95.2° about each bone's own local +Z, with the four fingers
  agreeing to within 0.6°, plus a half-grab at +24/+43/+46. **A grip solver does not have to invent
  its curl curve or its joint limits.** The thumb is the exception: it moves on more than one axis.
- **G-01 is not buildable as written.** Cards have no grip point and no `grip` keyword, and
  `vr_card.h:95` is `grab[VRJ_MAX_JOINTS][3]` — indexed by *joint*, so it structurally cannot hold a
  hand grip. `vrcontrol_t` has no radius, which is the quantity the whole `controls_under` rule is
  written in terms of. There is no analog grip value: both grip actions are `XR_ACTION_TYPE_BOOLEAN`
  despite being bound to `/input/squeeze/value`, and the wire carries one bit. `VRBTN_THUMB_TOUCH` is
  defined and never written. The engine has **never read a card** — it needs a game-API v5 query
  before it can know where a control is.
- **"Blocks every weapon control on every gun" overstated it by about a factor of nine.** Two of the
  25 retail-bound cards declare a control, both at placeholder `0 0 0`. Part G's table — safety,
  selector, magazine release, slide stop, decocker, cylinder latch, hammer — is authored on zero
  weapons. Control positions are content work and nobody has done any of it.
- **`R_StudioApplyHandAction` cannot carry a hand pose.** Eight slots against 19–26 bones, and the
  payload is a scalar, not a rotation. Part B's "no new renderer API" holds for joints and not for
  hands.
- **There is no triangle walker in `engine/`.** `mod_surgery.c` walks bodyparts, submodels and
  vertices only. Every live triangle walk is in the renderer DLL.
- **Header bounding boxes are not merely unreliable, they are all zero** on every retail valve model,
  and the `Mod_StudioComputeBounds` fallback bounds raw bone-space vertices without applying any bone
  matrix. Measure extents from vertices.
- **`tools/vrcard/CENSUS.txt` is not a map of what the shipped cards bind to.** It censuses retail;
  the twelve `cards/valve/` cards do not.

## Cards: what binds to what

Measured by recomputing every fingerprint against every model set on disk.

| Card set | Binds to |
| --- | --- |
| `cards/gearbox/` (7) | **retail Opposing Force**, 7/7. All are weapons with no HD variant. |
| `cards/hd/` (18) | **retail `valve_hd` + `gearbox_hd` + `bshift_hd`**, 18/18 fingerprints verified. |
| `cards/valve/` (12) | the **Half-Life VR Mod's** rigs. **0/16 retail Half-Life.** Useless on a stock install. |

Retail coverage, since the player brings their own install:

- **18 HD fingerprints** cover 15 weapons; 13 of those 18 serve all three games from one card, because
  the HD rigs are shared. Only the pistol, crowbar and snark fork, where Blue Shift ships its own.
- **12 weapons have no HD model at all** (the Opposing Force specifics plus `v_chub`,
  `v_desert_eagle`, `v_m40a1`). Seven are carded; five are not: `v_bgrap`, `v_bgrap_tonguetip`,
  `v_chub`, `v_desert_eagle`, `v_spore_launcher`.
- **Minimal complete set: 30 cards.** 25 written, 6 of the 18 HD ones verified clean.
- Supporting SD installs as well would take the full set to 67 distinct fingerprints.

Of the 18 HD cards: **6 ship clean** (`v_rpg`, `v_satchel`, `v_satchel_radio`, `v_shotgun`,
`v_squeak_bshift`, `v_tripmine`). The other 12 have fixes outstanding — one fatal (the synth/fingerprint
defect above), 22 serious and 51 minor, mostly comments asserting measurements that do not hold up.
Those comments matter: the next lane reads them as spec.

## Pick up here

In the order I would take them:

1. **Fix blocking defect 1** — the fingerprint/surgery conflict. Mesh surgery is how every weapon
   without modelled parts gets them, and it is currently mutually exclusive with card binding.
2. **Run it.** Nobody has launched the game and held a gun since the simulator became executable.
   Cheap, and it will certainly reorder everything below.
3. **Fix defects 2–5**, then land the 12 card fixes.
4. **Hands.** The card-format and input contract first (grip block, control radius, an analog grip
   action, game-API v5), then the solver — measuring its curl curve from `fullgrab_start` rather than
   inventing it.
5. **Control positions**, per weapon. Parallelizable content work, and nothing in Part G's table
   exists without it.
6. **Part I — the world and the body.** Holsters, dropping, picking up, the pouch. This is what makes
   a reload feel like a reload, and what makes the carded throwables throwable. Wants a physics library.
7. **Part H sights, Part L malfunctions, haptics.**
8. **Netplay hardening.** Split authority is the chosen design. 56 risks were found in shipped code;
   the two sharp ones are **signed overflow on stock defaults** in the joint integrator
   (`vr_joint.cpp:324`, undefined behaviour, which is what fixed-point existed to prevent) and a card
   parser with **no range validation at all** — which matters precisely because mod-friendly means mod
   authors typing numbers.

## One more fun thing per gun (owner, 1 Oct)

The owner's note, kept because it is the right instinct and the architecture is
already most of the way there: **examine each weapon for one more thing a hand
can do with it.** The example that prompted it - the shotgun's stock could be
deployed, and that could mean less muzzle climb and slower handling.

Mechanically this is cheap. A stock is a JOINT: a bone, a travel, a detent at
"deployed". The card format expresses that today. What does not exist is the
CONSEQUENCE - a way for a joint's position to change how the weapon handles.
Part F already carries recoil and inertia for the held weapon, and a stock
against the shoulder is a third contact point, which is what two-handed
stabilisation already models. So the addition is one general card concept -
"while this joint is past this detent, scale recoil/sway by K" - and it serves
every weapon rather than being a shotgun special case.

Candidates worth measuring the models for, in rough order of how much they
would add:

- **shotgun, MP5** - fold out the stock. Less climb, slower to bring on target.
- **revolver** - thumb the hammer for single action: slower, steadier, and it is
  already a declared joint with a hammer_cock detent.
- **pistol** - a press check. Draw the slide a quarter inch and see brass.
- **crossbow** - flip the scope up out of the way.
- **gauss, displacer** - the spinners are already hand-turnable; make spinning
  them mean something.
- **RPG** - flip the sight up.
- **tripmine, satchel** - the arming state as a thing you can see and change.

Nothing here is on the critical path. It is what the project is FOR, so it is
written down rather than remembered.

## Decided, so do not re-litigate

- **Fidelity default: standard. Released weapons: return to holster.** (Owner, 29 Sep.)
- **Grenades: one mechanism, both behaviours.** The pin starts nothing; your grip holds the spoon;
  a thumb control releases it deliberately, which is cooking, and throwing releases it too.
- **Prying: dropped.** There is nothing in Half-Life to pry.
- **Haptics: the platform has them.** `VR_Haptic` is implemented and fired from eight call
  sites - weapon fire, impacts, a magazine seating, a cylinder latching. What is deferred is only
  Part G's per-mechanism table (a detent clicking, a catch engaging), which is written and unbuilt.
- **Retail content is the target.** We ship the engine, the player brings the game. (Owner, 1 Oct.)
- **HD rigs preferred where they exist**, our own VR-mod rips where they are better and we have them.
  (Owner, 1 Oct.)
- **Single player runs the same wire as a network game.** One path, not two, so every session tests
  the netplay path. (Owner, 1 Oct.)
- **Netplay state: split authority.** Feel state client-local and never corrected; consequence state
  authoritative as discrete events. Won 7.0 against an authoritative state channel at 6.7 and pure
  input determinism at 5.3.

## Traps already paid for

Each of these cost a wrong implementation first.

- **A catch being *engaged* is not the joint *resting* on it.** The slide stop engages while the
  slide is still travelling back, nowhere near the notch.
- **The hand needs a *demand* and a *clamped force*.** Physics uses the clamp; catches read the
  demand. Once both a tentative pull and a firm tug clamp to the same force, "a firm tug past the
  stop" is inexpressible.
- **Detent crossings need hysteresis.** Without it a hand resting on a mark crosses it ten-plus
  times a second.
- **Part F wants a *time*, not a stiffness.** A spring stiff enough to hold a pistol within 2 mm is
  unstable at any sub-step a 90 Hz frame can afford.
- **Recoil belongs to the cartridge.** Derive both recoil and inertia from mass and every term
  cancels: a launcher kicks exactly as hard as a pistol.
- **One bone cannot be two joints.** A part is a lerp between two bracketed poses driven by one
  value, which is why the M40A1's bolt had to be carved and its handle rewritten.
- **Joint order is not arbitrary.** The `magazine_slide` preset makes joint 0 the action and joint 1
  the magazine; numbering them otherwise silently swaps their catches.
- **Fire rates on hand-worked weapons want to be loose.** The mechanism is the limiter; a tight rpm
  adds an invisible second timer that refuses to fire when the player has done everything right.
- **A module passing its own tests says nothing about whether it is connected.** Part J's whole strike
  module was referenced by both DLL builds and never committed; the determinism hash could not see the
  grenade; client prediction had never executed. Each passed every test it had, because the tests
  called the module directly and never went through the game.
- **A test with a fixed expected value can be vacuous.** The hash-coverage test's own first draft set
  two fields to values `VRGun_Init` already left them holding. Mutate by increment, not assignment.
- **Verifying a build in a tree that holds untracked files proves nothing** about the tree anyone else
  clones. `tools/audit_build_refs.py` now checks every build reference against git.
- **`if errorlevel 1` does not catch a crash.** It means "errorlevel >= 1", and an access violation
  exits -1073741819. The suite runner printed every case, faulted, and said "all suites pass". Test
  exactly for zero. `if exist <exe> <exe>` lies the same way: an absent executable runs nothing and
  leaves ERRORLEVEL alone, so it reads as a pass.
- **Fix every instance, then look for a fifth.** The spring defect had four. The first pass fixed
  two, and the two it missed were the two whose own cards already documented the behaviour the engine
  did not implement - which is what a half-done sweep looks like from the outside.
- **Removing a force means replacing what it held.** Taking the cylinder's return spring away made the
  detent's hysteresis band a reachable resting place, because the open flag is latched from that
  crossing - so a half-closed cylinder read shut and would not fire. A latched flag's mark must sit
  where a hand does not leave the part.
- **No spring and no damping is not a mechanism, it is a projectile.** Nothing dissipates energy, so
  any nudge coasts to a travel limit.

## How to check it still works

```
hlsdk-portable\dlls\vr_test_all.bat               117 cases, seven suites
hlsdk-portable\dlls\vr_determinism_matrix.bat     nine builds, 12,000 commands, x87 included
hlsdk-portable\dlls\vr_determinism_test.bat       the original x86-vs-x64 pair
XashFWGS\tools\vrcard\vr_hold_test.bat            Part F's claims, measured
XashFWGS\tools\audit_build_refs.py . ..\hlsdk-portable
XashFWGS\tools\vrcard\mesh_completeness.py <gamedir>
XashFWGS\tools\vrcard\vrcardgen.py --census <dir>
hlsdk-portable\dlls\vr_card_check.bat <card>      what the simulator will see
```
