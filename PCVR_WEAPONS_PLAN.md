# XashPCVR Weapons — The Full Build

Sep 29, 2026 · @Simon

Every weapon in Half-Life and Opposing Force becomes a simulated machine: parts that move on real joints, rounds that travel through the gun as objects, a weapon with mass that collides with the world, and hands that are the only thing operating it. This is the owner's top priority and is committed in full. It replaces the verb-based weapon plan in Part E of *XashPCVR — Platform Direction and Architecture*; the hand-event channel, the census and the netplay rules from that plan all carry over.

# Part A — Scope

Everything below is committed. Each item has an ID so commits, tests and the feel checklist can cite it. "Done" means working in the headset on Half-Life and Opposing Force, predicted correctly, and listed as passing in Part O.

**Mechanisms (Part C)**

- [ ] **M-01** Every moving part is a joint: a slider (slides, pumps, bolts, magazines, rockets) or a hinge (cylinders, break-actions, triggers, hammers, levers), with limits, a spring, damping and optional catches.
- [ ] **M-02** Detents and catches: slide lock on empty, magazine latch, cylinder chamber clicks, bolt handle up/down.
- [ ] **M-03** Two-axis bolt action (lift, back, forward, drop) for the M40A1.
- [ ] **M-04** Controls on the gun operated by the thumb or finger at their real location: safety, fire selector, magazine release, slide stop, decocker, revolver hammer, cylinder latch.
- [ ] **M-05** Trigger with travel and a break point; half-press does nothing, the break fires.
- [ ] **M-06** Recoil drives the action on self-loaders: the slide or bolt is thrown back by the shot, not by an animation.

**Rounds (Part D)**

- [ ] **R-01** Every round is tracked: in a magazine, in a tube, in a cylinder chamber, in the gun's chamber, in the hand, or in the world.
- [ ] **R-02** Racking a loaded gun ejects the chambered round as a live round that can be picked up and loaded again.
- [ ] **R-03** Press check: ease the action back and see brass in the chamber.
- [ ] **R-04** Spent cases are physical, bounce, and sound right on the surface they land on.
- [ ] **R-05** Dropped magazines keep their rounds and can be picked up and reused; a partial magazine reports its count.
- [ ] **R-06** Shotgun: load through the loading gate, or drop a shell into the open ejection port and close the action.
- [ ] **R-07** Shell holder on the shotgun, if the model allows it (synthetic attachment otherwise).
- [ ] **R-08** MP5 grenade launcher loaded by hand.

**Content (Part E)**

- [ ] **E-01** Cards declare joints, not verbs; derived from the model by the generator, corrected by hand.
- [ ] **E-02** Load-time mesh surgery: a part defined by a vertex selection gets its own bone, so parts the animators never made exist (MP5 charging handle first).
- [ ] **E-03** Cards carry each part's geometry in the weapon's own frame, so game code knows where parts are without the renderer.

**The weapon as an object (Part F)**

- [ ] **F-01** The held weapon is a rigid body with mass and inertia, held by spring constraints to the hands.
- [ ] **F-02** Recoil is an impulse at the muzzle; muzzle climb and one-handed instability come from physics.
- [ ] **F-03** A second hand and a shouldered stock steady it.
- [ ] **F-04** The weapon collides with the world: walls push it back and it cannot fire through them.
- [ ] **F-05** Weapons can be set down, dropped and picked up, and stay where they land.
- [ ] **F-06** Racking against a surface: catch the sight on a table edge and push.

**Hands (Part G)**

- [ ] **G-01** Procedural grip: fingers close until they touch the weapon.
- [ ] **G-02** Trigger discipline: index finger along the guard until the trigger is touched.
- [ ] **G-03** Fingers follow analog grip and trigger; optional OpenXR hand tracking.
- [ ] **G-04** Haptic detents from mechanism events; recoil and charge haptics.

**Sights (Part H)**

- [ ] **H-01** Iron sights aligned physically.
- [ ] **H-02** Red-dot style reticles drawn at infinity.
- [ ] **H-03** A real magnifying scope on the M40A1 with eye-relief shading.
- [ ] **H-04** Laser sights where the weapon has one (RPG guidance, Desert Eagle).

**The world and the body (Part I)**

- [ ] **I-01** Body inventory: holsters on hip, chest and shoulders placed from the body solve.
- [ ] **I-02** Wrist display on the HEV suit: health, armour, ammo.
- [ ] **I-03** Dual wield any one-handed weapon, each with its own state.
- [ ] **I-04** Ammo pouch hands you the right thing for the gun in your other hand.

**Arsenal (Parts J and K):** every Half-Life and Opposing Force weapon specified there, including grenades with pin and spoon, satchel detonator, placed tripmines, a snark that bites, Gauss charge and Gauss jump, crowbar prying, and the Opposing Force M40A1, SAW, Desert Eagle, displacer, spore launcher, shock roach, knife, pipe wrench, barnacle grapple and penguin.

**Options (Part L)**

- [ ] **L-01** Malfunctions (stovepipe, failure to feed) with tap-rack clearing; off by default.
- [ ] **L-02** Fidelity settings per mechanic: arcade, standard, full.

**Multiplayer (Part M):** all of the above predicted and server-authoritative; others see your weapon's parts, your dropped magazines and your hands.

# Part B — Architecture

&#91;embedded content: weapon architecture · engine, shared code, server physics\]

The split rule: **anything that changes the game state is decided in shared weapon code; anything that only moves or draws the gun lives in the engine.** Where the gun is in space is the engine's business, like the player's view angles have always been. What the gun does — which round is where, whether it fires — is game code's, run identically by prediction and the server.

**What changes from today.** Today the engine drives part positions from the hand and sends them (`VR_FillCmd`, `part_value[]`), and `vr_weapon.cpp` derives events from those values. In the full build the engine sends **the hands**, expressed in the weapon's own frame, and the shared code runs the joint simulation itself: it decides where the slide is. The engine's renderer then reads the predicted joint values back from the client DLL. One source of truth for every part, and it is the one the server also runs.

**Posing stays in the engine.** The VR layer asks the client DLL for the predicted joint values through the existing game interface (`GetVRWeaponAPI`), writes them into `refState.vrParts[].value`, and the existing `R_StudioApplyHandAction` poses the bones. No new renderer API, and no patch to each mod's own studio renderer. Uncarded weapons and carded ones share one posing path, including the magazine shrink, re-posing child bones and the lighting-chain fix.

That means game code must know where parts are without the renderer. So cards carry each part's geometry in the weapon's frame — grab point, axis, travel, extent — measured once by the generator (Part E), instead of the renderer re-deriving it every frame.

**Pieces and where they live**

| Piece | Lives in | Why there |
| --- | --- | --- |
| Tracking, body solve, grip solver | engine client (`vr_openxr.c`, split into files) | Local, per frame, no game state |
| Held-weapon rigid body, hand springs, weapon-vs-map collision | engine client, new physics module | Local, like view angles; its result (gun pose, muzzle) goes up in the command |
| `vrcmd_t` v2 | `common/vrcmd.h`, both engine and game code | The only input shared code gets |
| Card loader and generator | engine (loads, generates) + shared code (reads the parsed card) | Generator needs models; game code needs geometry |
| Joint simulation, rounds, controls, trigger, fire permission | shared weapon code, `hlsdk-portable/dlls/vr_weapon*.cpp` | Must be predicted and authoritative |
| Mesh surgery | engine model loader (`mod_studio.c`) | Changes the model before anything draws it |
| Renderer posing, scopes, reticles | ref/gl | Drawing only |
| World rigid bodies (dropped magazines, rounds, weapons) | engine server, same physics module | Authoritative objects everyone sees |
| Casings | engine client, same physics module | Cosmetic; never networked |

**Physics library.** Use one library on both sides. Jolt Physics (C++17, MIT licence, deterministic mode available) is the recommendation; HLVR used ReactPhysics3D, which is also viable. Wrap it behind a small C interface (`vrphys_*`) so the C engine never includes C++ headers, and build it into the engine for both the client and the dedicated server. World collision comes from the map's own brush geometry, built into a static mesh at map load; brush entities (doors, trains, lifts) become kinematic bodies that follow their entities.

# Part C — The mechanism simulator

A weapon is a small set of joints. Each joint has one degree of freedom, and every behaviour in the feel checklist is a property of a joint, a catch or a rule that connects two joints. This replaces `VRWeapon_Update`'s threshold logic; its hysteresis, re-arm and baseline rules survive as joint properties rather than special cases.

**Joint**

| Field | Meaning |
| --- | --- |
| `type` | `slide` (linear) or `hinge` (rotary) |
| `bone` | the model bone it poses (real, or created by mesh surgery) |
| `axis`, `origin` | in the weapon's frame; from the card |
| `travel` | units or degrees from rest to full; measured by the generator |
| `rest` | 0 or 1 — where the spring returns it (a pump rests forward, a cylinder rests closed) |
| `spring`, `damping`, `mass` | how it returns and how it feels in the hand; per-type defaults, card overrides |
| `grab` | grab point and radius in the weapon's frame, plus the hand pose to use |
| `catches[]` | positions where the joint can be held (see below) |
| `detents[]` | positions that click as the joint passes: small resistance, a haptic tick, a sound |
| `recoil` | self-loaders only: velocity given to the joint when a shot fires |
| `gate` | another joint's condition that must hold before this one moves (the bolt cannot slide until its handle is lifted) |

**Catch.** Holds the joint at a position against its spring. Declared as `at`, `holds` (which direction it resists), `engages` (a rule: "magazine empty and joint passes behind 0.95" for a slide lock) and `releases` (a control, a tug past a threshold, or both). A magazine latch is a catch on the magazine joint that engages at 0 and releases only by the magazine-release control — or, if the owner prefers, by a firm pull (fidelity setting).

**Control.** A button, lever, toggle or selector at a real location on the model, operated by a named finger of whichever hand is on the gun there. Safety, fire selector, magazine release, slide stop, decocker, cylinder latch, revolver hammer. The engine reports which controls a finger is on and pressing (Part G); the simulator applies their effect.

**Hand drive.** A held joint is pulled toward the hand's projection onto its axis by a stiff spring with a **force limit**. The limit is what makes catches feel real: an ordinary pull stops at a catch, a firm tug past the stop exceeds the catch's hold and releases it. Letting go leaves the joint to its spring and catches — so "a slide nobody is holding never closes on its own" and "letting go at the back still counts" are both just physics.

**Events** are crossings: `(joint, position, direction)`. "Slide passed 0.9 going back" ejects; "slide passed 0.4 going forward" strips and chambers a round; "magazine passed 0.1 going in" seats it. Crossings drive the feed path (Part D), haptics, sounds and the old HL weapon code. A crossing replayed from the same state produces the same crossing once, so prediction is safe by construction.

**Trigger.** A hinge joint driven directly by the analog trigger. The sear breaks at a declared position (default 0.7) and resets below another (0.3). A shot is fired when the sear breaks and: the chamber holds a live round, the hammer or striker is cocked, the action is in battery (at rest within tolerance), and the safety is off. Full-auto keeps firing while the trigger stays past the break, at the weapon's own fire rate. For HL weapon code, a shot is delivered by synthesising `IN_ATTACK` for that command, and the round comes from our chamber rather than `m_iClip`.

**Keeping HL code honest.** `m_iClip` is written every tick as rounds in the magazine plus the chamber, so the HUD, `CurWeapon` and any code that reads it stay correct. For a hand-loading player, the weapon's own `Reload()` and automatic reload never run; the simulator owns loading.

**Determinism.** The simulator runs once per command with `dt = cmd->msec`. Joint positions and velocities are **fixed-point** (32-bit, 16 fractional bits of travel), not float, so the client DLL and the server DLL — separate binaries, possibly different compilers — produce bit-identical results. No clock, no globals, no random except the shared seed, no cvars except per-player userinfo.

**Quantise at fill time, never in the encoder.** `vrcmd_t` holds only integers in wire units (1/64 unit positions, smallest-three rotations, 8-bit analogs). The engine quantises when it fills the block, and the simulator only ever sees those integers — on the server, in client prediction and in a local game alike. If the block held floats and only the network encoder quantised them, prediction would run on different inputs than the server and the mismatch counter would never reach zero.

**Fixed sub-steps.** The simulator steps at a fixed 2 ms and carries leftover milliseconds to the next command; the leftover is part of the predicted state. Within a command the hand target moves smoothly from the previous command's quantised pose to this one's across the sub-steps. Slide behaviour is then the same at 45 fps and 90 fps.

**Prediction state.** Prediction rewinds weapon state to the last server snapshot and replays commands, so the simulator's whole state travels in weapon data: up to 8 joints × (position, velocity) plus catch bits, controls and the round state from Part D — about 160 bytes per active weapon, delta-compressed, only for hand-loading players, under the `NET_EXT_VR` capability.

**Performance.** A handful of joints per weapon at command rate: negligible.

# Part D — Rounds and the feed path

Every round has a place, and the joints in Part C move rounds between places. Nothing appears or disappears except by a crossing or a hand.

**Places a round can be**

| Place | State kept | Notes |
| --- | --- | --- |
| Magazine (seated) | count, ammo type | a magazine is an object with its own count; the seated one lives in the weapon |
| Tube | count | shotgun |
| Cylinder | per chamber: empty / live / spent; the index under the hammer | revolver |
| Belt | count, link state, cover open/closed | SAW |
| Chamber | empty / live / spent | every firearm |
| Hand | nothing / round / shell / magazine (with count) / speedloader (with count) / bolt / rocket / grenade | at most one item per hand |
| Pouch | the player's reserve (`m_rgAmmo`) plus up to four partial magazines per ammo type | reaching the pouch always yields the right item for the gun in the other hand, fullest first |
| World | live rounds, magazines and weapons as server entities; spent cases as client-only debris | see Part I |

**Feed rules** (crossings from Part C trigger each step):

- **Fire:** chamber live → spent. A self-loader's action gets its recoil velocity and cycles itself through the rules below.
- **Action back past the ejection point:** chamber contents leave through the port — a spent case becomes debris; a live round becomes a pickup (**R-02**, racking a loaded gun loses the round unless you catch it).
- **Action forward past the feed point:** if the magazine or tube has a round and the catch is not engaged, the top round moves to the chamber.
- **Action back with an empty magazine** on a weapon with a slide lock: the catch engages and holds it open.
- **Press check (R-03):** easing the action back short of the ejection point exposes the chamber; the renderer draws the chambered round's model at the chamber position declared in the card.
- **Loading gate (tube):** a shell carried to the gate goes into the tube, one per insert.
- **Ejection port (R-06):** with the action held open, a round dropped into the port goes straight to the chamber; closing the action keeps it there.
- **Magazine out:** the seated magazine becomes the hand's item (if pulled by hand) or falls (if released by button with no hand on it), keeping its count. One round stays in the chamber.
- **Magazine seated:** the hand's magazine becomes the seated one. The chamber is **not** loaded until the action is worked or the slide is released — except on weapons whose card says `chamber_on_seat` (the MP5 with no bolt, until mesh surgery gives it one).
- **Revolver:** open the latch, swing the cylinder; muzzle up plus the ejector pushes every case out (spent become debris, live become pickups); a speedloader fills every empty chamber at once, single rounds fill one; closing aligns the cylinder. Thumbing the hammer advances one chamber and cocks it; a double-action pull does both and fires.
- **Single shot:** crossbow — draw the string to its catch, lay a bolt in the groove, fire releases the catch. RPG — slide the rocket into the tube to its latch, fire launches it.
- **Belt (SAW):** open the top cover, lay the belt on the feed tray from the box, close the cover, work the charging handle.

**What the player sees.** The magazine shows it is loaded (its top round model is visible if the model has one, drawn if the card declares a position if not). An open cylinder shows live and spent chambers. A partial magazine in the hand shows its count on the wrist display (Part I) while held.

**Pickups are physical.** A live round, magazine or weapon in the world is grabbed by closing a hand on it (Part I): no walk-over pickup for hand-loading players, except that classic ammo boxes still add to the pouch reserve so Half-Life's own ammo placement keeps working.

**HL ammo accounting stays whole.** Taking a magazine from the pouch subtracts its count from `m_rgAmmo`; dropping one and leaving it loses those rounds; picking it up returns it. Ammo boxes, weapon pickups and the HUD behave as stock.

# Part E — Cards v2 and mesh surgery

## Cards declare joints

A card now describes the weapon's joints, catches, controls and the fixed points of its feed path, all in the weapon's own frame. A firearm type (`magazine_slide`, `tube_pump`, …) is a **preset** that fills in the usual joints and rules; the card only states what differs. The SAW and the shock rifle, which fit no type, simply declare their joints directly.

**Binding: by fingerprint, not by bone presence.** The coder's brief applies the first card whose bones the model has. That will misfire: `Bone01` and `Bone02` are generic 3ds Max names present in many rigs, so Blue Shift's shotgun (pump on `Bone02`) would match the SD card (pump on `Bone01`) and drive the wrong bone. A card therefore records what it was measured from — bone count, sequence count, a hash of the bone names, and each joint's measured travel — and applies only when the model matches within tolerance. The census already has every one of these numbers.

**Location.** `<gamedir>/vr/cards/<model>.card`, bound to the model file, with generated cards written to `vr/cards/generated/` for review and promotion. SD and HD under one `valve` directory stop being a problem, because each card binds to the model it was measured from.

**Sketch** — Half-Life SD pistol, bones and travels from the census; positions shown as placeholders the generator fills in:

```
weapon v_9mmhandgun
    match     bones <n>  seqs <n>  hash <h>
    type      magazine_slide
    joint     slide     bone "Bone02"   slide  travel 1.45  rest 0
        catch lock      at 0.95  engages mag_empty  releases slide_stop tug
        grab            point <x y z>  radius 2.5  pose pinch
    joint     magazine  bone "Box02"    slide  travel 21.10 rest 0  out_at 0.35
        catch latch     at 0.02  releases mag_release
    control   mag_release   at <x y z>  finger thumb  kind button
    control   slide_stop    at <x y z>  finger thumb  kind button
    chamber   at <x y z>
    eject     at <x y z>  dir <x y z>
```

`out_at` exists because a magazine's measured travel is how far the reload animation carries it — 21 units for this pistol — while it is actually free of the well much sooner. Every measured number can be overridden this way.

## The generator

The census rules are the generator's rules: search every sequence, never just the one named after the action; exclude the body bone (a root-parented bone owning more than about 40% of the mesh); exclude MMod's `camera_bone`; treat bones owning 3–10% of vertices as candidate parts; and be allowed to return nothing. Then, for each part, measured **relative to the weapon root, not its parent**:

- **Axis and joint type:** the dominant direction of the part's displacement (slide) or its rotation axis (hinge).
- **Role:** along the barrel above the grip → slide or bolt; along the barrel ahead of the grip → fore-end; across the barrel and downward out of the gun → magazine; rotating about an axis parallel to the barrel → cylinder; appearing only in reload and travelling in from outside → round, rocket or loader. "Forward" comes from the muzzle attachment.
- **Geometry:** grab point (centroid of the part's vertices at rest, in the weapon frame), radius, travel, and the chamber and ejection positions (from the shell-eject position HL's client events already use, refined by the card).
- **Fingerprint:** as above.

It runs at weapon load when no card matches, logs its guess once, and writes the card it made. A guessed card never drives a catch or a control it is not confident of; when unsure it produces the fallback, not a wrong mechanism.

## Mesh surgery (E-02)

The content ceiling is parts that were never separate bones: the MP5 has no bolt or charging handle, and many magazines, hammers and levers are welded to the body. Mesh surgery gives them a bone at load time.

**Declared in the card:**

```
synth charging_handle
    verts   bone "<body bone>"  box <x0 y0 z0> <x1 y1 z1>
    pivot   <x y z>
    joint   slide  axis <x y z>  travel 1.2  rest 0
```

Vertices can be selected by box, by texture name, by the bone they currently belong to, or a combination.

**Done in the engine model loader** (`engine/common/mod_studio.c`), on client viewmodels only, before anything draws them:

1. Load the model as usual, then build a **new** studio buffer with one more bone per synthetic part (limit: `MAXSTUDIOBONES`).
2. Append the bone at the end, so every existing bone index, hitbox and attachment stays valid. Parent it to the bone most of the selected vertices belonged to, with its origin at the declared pivot.
3. Move the selected vertices (and their normals) onto the new bone, re-expressed relative to the pivot. Weighted models (`STUDIO_HAS_BONEWEIGHTS`) have their weights rewritten to match.
4. Every sequence needs animation data for every bone, so rebuild each sequence's animation block with one extra entry whose values are the bone's rest pose.
5. Hand the new buffer to the renderer. The client DLL's own studio renderer reads bones through the engine, so it sees and animates the new bone without knowing anything happened.

**Step 4 needs no re-encoding.** The new bone's animation entry is all zero offsets, which GoldSrc reads as "use the bone's default value". For each sequence and each blend, write the grown entry array into the new buffer, copy the compressed animation data after it untouched, and recompute every non-zero offset as (target's new address − entry's new address). The only decoding needed is finding where each sequence's data ends. Refuse the model if any offset would exceed 65,535 or if it uses external sequence-group files. Headroom: the largest viewmodel in the census has 83 bones against a limit of 128.

The census tool gets a mode that previews a selection, so a synthetic part is defined by looking at the model, not by guessing coordinates in a headset.

**First targets:** the MP5 charging handle (so the MP5 gets a real `magazine_bolt` card and loses `chamber_on_seat`), the pistol's slide stop and magazine release, the revolver's hammer and cylinder latch, and the shotgun's loading gate.

# Part F — The held weapon as a physical object

Today the weapon is pinned to the controller. In the full build it is a rigid body the hands hold through springs, simulated on the client every frame, and its resulting pose is what gets drawn and what aims.

**The body.** Mass and inertia come from the model: the body bone's geometry gives a volume and a centre of mass; a density per weapon class turns volume into mass. The card can override both. Heavy weapons (RPG, SAW, Gauss) end up heavier than pistols without anyone tuning them.

**Hand constraints.** Each hand holding the weapon is a six-degree-of-freedom spring-damper from the controller's grip pose to a grip point on the weapon: the dominant hand to the pistol grip, the off hand to whichever support point it is on (fore-end, magazine well, second grip — from the card). Critically damped, simulated at display rate with four sub-steps. Stiffness is set so a pistol follows the hand within a millimetre or two and a rocket launcher visibly lags a little. When the stock is near the shoulder anchor from the body solve, a third, softer constraint holds the stock to the shoulder: a shouldered rifle is steadier because it physically is.

**Recoil (F-02).** A shot applies an impulse at the muzzle along the barrel and a torque that lifts it; the springs bring it back. Muzzle climb, two-handed control and shoulder bracing all come from the same numbers. One-handed with a heavy weapon, the impulse can exceed the hand's force limit, so the grip slips and resettles over a few tenths of a second — a one-handed shotgun blast nearly leaves the hand. The camera never moves.

**Collision (F-04).** The weapon's collision shape is a capsule from grip to muzzle plus a box for the receiver, sized from the model. It collides with the map (a static mesh built from the map's brushes at load) and with moving brush entities. A wall stops the barrel; the gun pivots in the hand instead of passing through. Monsters and players do not push the gun in v1.

**Hands when the gun is blocked.** Once the gun is held away from the controller by more than a few units, draw the hand on the gun and a faint outline at the controller, so the player sees why their aim stopped. The separation limit that makes the gun let go entirely (a snag, not a drop) is a setting.

**Aim comes from the simulated gun.** The muzzle pose sent in `vrcmd_t` is the post-physics, post-collision pose. The laser sight, grenade arc and scope all use it. The server checks that the muzzle is reachable from the player (a line from the player's eye to the muzzle must not cross the world); if it is not, the shot starts at the point where that line hits the wall. You cannot poke a barrel through a door and fire, locally or online.

**Racking against a surface (F-06).** A card can mark a point as a rack surface (usually the rear sight). When that point is in contact with the world and the gun is pushed toward it, the engine reports it as an external drive on the slide joint — a third "hand" in `vrcmd_t`. The shared simulator treats it exactly like a hand pushing the slide, so the rack is predicted and authoritative like any other.

**Letting go (F-05).** Opening the dominant hand when the gun is not at a holster releases it. The engine sends the release with the gun's pose and velocity; the server removes it from the player's active weapons and spawns it as a physical world object carrying its full mechanism and round state (Part I). With the "return to holster" setting on (the default in standard fidelity) it snaps back to its holster instead.

**Comfort limits.** Recoil never moves the camera. Weapon lag is capped: nothing lags the hand by more than about 3 cm, however heavy. Both limits are settings, and both go on the feel checklist.

# Part G — Hands on the gun

**Grip solver (G-01).** When a weapon loads, solve a closed hand for each grip point in its card: for each finger, rotate its joints from open toward closed until the finger's segments touch the weapon's geometry (the rest-pose triangles of the body and any parts at that grip). Cache the result per weapon and grip. At run time, blend from the open pose to the solved grip by the analog grip value. This works with the current `.mdl` hands (19 finger bones, no sequences — built to be posed) and with the IQM hands when they arrive, and needs no per-weapon authoring.

**Index finger and trigger discipline (G-02).** Three states from the controller's trigger-touch sensor and trigger value: off the trigger (finger straight along the guard, solved against the guard), touching (finger on the trigger face), pulling (finger follows the trigger joint's travel). Controllers without a touch sensor use trigger value above a small threshold as "touching".

**Thumb and controls (M-04).** The thumb rests where the grip solve puts it unless the thumb-touch sensor says it has moved. Each control in the card has a location and a finger. When the thumb's solved tip is within a control's radius, that control is "under the thumb" and the controller input takes it over. Starting mapping, for the owner to tune in the headset:

| Control | Operated by |
| --- | --- |
| Magazine release, slide stop, cylinder latch, decocker | face button (A / X) while the thumb is on it |
| Safety, fire selector | thumbstick flicked up or down while the thumb is on it |
| Revolver hammer | thumbstick pulled back while the thumb is on it |
| Anything else, thumb elsewhere | the button's ordinary binding |

Whichever control is under the thumb gets a subtle highlight and a haptic tick on arrival, so the player can find the magazine release without looking.

**Off hand on parts.** The off hand grabs joints (Part C), rounds, magazines and world objects (Part I). Its pose while holding a joint is the grab pose named in the card (pinch for a slide, wrap for a fore-end, palm for a magazine base).

**Hand tracking (G-03).** Optional, when the runtime offers OpenXR hand tracking: finger joints drive the hand directly, pinch replaces the grip button, and the trigger is a curl of the index finger. Controllers stay the default.

**Haptics (G-04).** Every mechanism event has a pattern, played on the hand that felt it (and more softly on the other when both are on the gun):

| Event | Pattern |
| --- | --- |
| Detent passed | very short, light tick |
| Catch engages or releases | short, firm |
| Magazine seats | firm, slightly longer |
| Round chambers | short, medium |
| Shot | strength and length by weapon class, split between hands by how much each is holding |
| Gauss charging | ramping continuous buzz |
| Egon firing | steady continuous |
| Snark in the hand | irregular twitching, from the shared seed |

## What the engine sends: `vrcmd_t` v2

The command block grows from part values to hands. Sizes are starting points; the block is delta-encoded, so a still hand costs a few bits.

| Field | Size | Meaning |
| --- | --- | --- |
| Per hand: pose | 6 × 16 bits | position (1/64 unit) and rotation (smallest-three quaternion), in the weapon's frame when on the weapon, in the player's frame otherwise |
| Per hand: analog | 2 × 8 bits | grip, trigger |
| Per hand: touch and buttons | 8 bits | trigger touch, thumb touch, face buttons, stick click |
| Per hand: stick | 2 × 8 bits | thumbstick |
| Per hand: velocity | 3 × 16 bits | for throws and releases |
| Per hand: holding | 8 + 16 bits | joint index, or world entity index |
| Per hand: carrying | 8 bits | item type and count |
| Controls under fingers | 16 bits | bit per card control |
| External drive | 8 + 16 bits | joint and target, for racking against a surface |
| Gun pose | 6 × 16 bits | world pose of the simulated weapon |
| Muzzle | existing | as today |
| Flags | 8 bits | released, shouldered, two-handed |

The game DLL still never sees an engine-side threshold: it gets where the hands are and what they are pressing, and decides everything else.

# Part H — Sights and scopes

**Never zoom the player's view.** Half-Life's crossbow and Opposing Force's M40A1 zoom by changing the field of view (`pev->fov`). In a headset that is nauseating and wrong. For hand-loading players the engine ignores field-of-view changes, and the weapon's zoom state instead sets the **scope's magnification**. The secondary-fire button cycles magnification the way it cycled zoom.

**Iron sights (H-01).** Nothing to invent: the weapon is drawn at true depth in both eyes (the flattened-depth mode stays off), and aligning the rear sight, front sight and target is something the player does with a steadied gun (Part F). The grip solver keeps fingers and thumbs clear of the sight line.

**Reticles at infinity (H-02).** For any lens the card declares (a rectangle on the model plus a reticle texture), draw the reticle on the lens **per eye**, at the point where the line from that eye to a far point along the barrel crosses the lens. The dot then sits on the aim point with no parallax, as a real red dot does. Stock Half-Life has no red-dot weapons; this is the capability for mods and for the scope's crosshair.

**The scope (H-03).**

- Render a second view from the objective lens along the barrel, with field of view = the lens's apparent size ÷ magnification, into a square texture (512–1024, a setting). One view shared by both eyes; the view is cheap because it is narrow.
- Draw it on the eyepiece disc. Compute the eye's position relative to the scope axis: centred within the eye box (eye relief and exit pupil from the card) shows the full image; off-axis slides a black crescent across it; outside the eye box shows black. Only the eye near the eyepiece sees the image; the other sees the scope's outside.
- Crosshair drawn in the texture, so it is exactly on the barrel line.
- Renderer work: an extra view pass before the eye passes, into an offscreen target, through a new ref API call. The R0 headroom work (single-pass stereo) pays for it.
- Targets: the M40A1 (two magnifications) and Half-Life's crossbow (the model carries a scope).

**Lasers (H-04).** The RPG's guidance laser follows the simulated muzzle, not the head — Half-Life already steers the rocket toward the laser dot, so this makes guided rockets hand-steered for free. The Desert Eagle's laser (Opposing Force secondary fire) is drawn from its muzzle with the existing laser renderer. The global laser-sight setting defaults to off on weapons whose card says they have usable sights.

# Part I — Things in the world, and on the body

## World objects

**Server physics.** A new move type, handled by the engine's server physics loop with the same physics module the client uses. Game code spawns an entity with that move type and the engine simulates it against the map and moving brushes; the result is networked as an ordinary entity (origin, angles), so every client sees it with no new protocol. Three kinds, all spawned by shared weapon code on the server:

| Object | Carries | Picked up by |
| --- | --- | --- |
| Magazine | ammo type, round count | closing a hand on it; goes to that hand |
| Live round, shell, bolt, rocket | ammo type | closing a hand on it |
| Weapon | its whole mechanism and round state | closing a hand on its grip; re-equipped exactly as it was left |

**Grabbing.** The engine finds the nearest grabbable entity within reach of a closing hand and puts its index in `vrcmd_t`. The server checks the claim: the entity must be within reach of that hand's reported pose plus a latency allowance. On success it attaches the entity to the hand (it follows the hand pose each command) until the hand opens (dropped with the hand's velocity), puts it in the pouch, or feeds it into a weapon. Classic Half-Life pickups — ammo boxes, weapons lying in the level, batteries, health — keep walk-over pickup so level design still works.

**Spent cases (R-04).** Half-Life already ejects shells on the client through its weapon events (`EjectBrass` in the client's event code), as temporary models that bounce. When the physics module is present, shell temporary entities are simulated as small rigid bodies instead, with surface-dependent sounds, capped at 64 live cases with the oldest removed first. The same path serves every client's view of every player's shots, and costs the network nothing.

## The body as inventory (I-01, I-04)

Slots hang off the solved body (neck, shoulder, chest and hip anchors), so nothing is tuned per player:

| Slot | Holds | Notes |
| --- | --- | --- |
| Dominant hip | a pistol or other one-handed weapon | release the weapon near it to holster; close the hand there to draw |
| Off-hand hip | the ammo pouch | always hands you the right item for the weapon in your other hand |
| Chest | grenades, satchels, tripmines, snarks | one grab = one item |
| Behind each shoulder | long guns; the crowbar | replaces the current hand-tuned shoulder hotspot |
| Off-hand wrist | the HEV display (I-02) | health, armour, rounds in the gun and in reserve; visible when the wrist turns toward the face |

Holster assignments are the player's own and live on their client; drawing from a holster sends the ordinary weapon-select command, so the server and every mod's weapon switching stay untouched. The existing weapon selector remains for everything not holstered.

## Dual wield (I-03)

Any weapon whose card marks it one-handed can be drawn into each hand. Game code gets a second active weapon for the off hand, with its own mechanism state, run each command from the off hand's side of `vrcmd_t`, and fired by the off hand's trigger. Reloading while dual wielding means holstering one gun or pinning it under an arm (a holster slot accepts it mid-reload). The engine already has a partial dual-wield path (`VR_DualWieldActive`, `VR_GetOffhandFire`); it is replaced by this.

# Part J — Half-Life's arsenal

Bone names and travels are the census's measurements for the SD rigs (shared by Opposing Force and Blue Shift, with the exceptions the census lists). "Synthetic" means a part created by mesh surgery.

## Firearms

| Weapon | Joints | Loading and handling |
| --- | --- | --- |
| **9mm pistol** | slide `Bone02` (1.45); magazine `Box02`; synthetic slide stop and magazine release | Magazine release under the thumb drops the magazine; seat a new one; slide-stop press or a tug on the slide sends it home. Locks back on empty. Press-check by easing the slide. |
| **.357 revolver** | cylinder `cylinder_spin` (30°); speedloader `Speedload Master`; synthetic cylinder latch and hammer | Thumb the latch, swing the cylinder out, muzzle up to dump, speedloader or single rounds, flick shut. Thumb the hammer for a single-action shot with a lighter trigger; or pull through double-action. Chambers advance one per cock. |
| **MP5** | magazine `Bone12` (16.71; `Bone03` in OpFor/BS); synthetic charging handle; synthetic grenade-launcher tube | Charging handle locks back on empty and is slapped home. Grenade launcher: take an AR grenade from the chest, slide it into the tube, off-hand trigger on the fore-end fires it. |
| **Shotgun** | fore-end `Bone01` (4.93; `Bone02` in BS); synthetic loading gate | Shells one at a time through the gate or dropped into the open port; pump to chamber. Double-barrel secondary becomes a two-stage trigger: pulling through a second stop fires both. Shell holder on the receiver if the owner wants one (synthetic). |
| **Crossbow** | string `Slide` (10.0); bolt rack `Clip Master` (8.4) | Draw the string back to its catch, lay a bolt in the groove, fire. Scope with real magnification (Part H). |
| **RPG** | rocket `rocket_master` (60.02) | Slide a rocket into the tube until it latches. Shoulder it. The laser follows the muzzle, so the rocket is steered by hand. |

## Energy and alien weapons

| Weapon | Handling |
| --- | --- |
| **Gauss** | Hold the secondary to charge: the barrel spinner (`Spinner`, 344°) spins up, haptics ramp, overcharge hurts as in Half-Life. Recoil pushes the player opposite to the **muzzle**, so the Gauss jump works by pointing the gun down and releasing (a comfort setting can soften it). No hand reload: it draws from the uranium reserve. |
| **Egon** | Continuous beam from the muzzle; the gun shudders in the hands through the physics. No hand reload. |
| **Hornet gun** | Living weapon: it recharges itself, twitches in the hand, and the hornets are visible on it. No hand reload. |

## Throwables and devices

| Weapon | Handling |
| --- | --- |
| **Hand grenade** | Take one from the chest. The off hand pulls the pin (`Bone04` ring, 139°; `Dummy23` in the pin-pull sequence). The grip holds the spoon; opening the hand releases it and starts the fuse in full fidelity (standard fidelity keeps Half-Life's fuse-from-pin cooking). Throw, lob or roll at hand speed; the arc shows where it will land. |
| **Satchel charge** | Take one from the chest; throw it or set it down gently where you want it. The detonator is a radio in the hand (Half-Life ships `v_satchel_radio.mdl`); press its button with the thumb. |
| **Tripmine** | Hold it to a wall: a ghost shows where it will sit. Press it to the surface and it attaches there, oriented to the surface, and arms. |
| **Snark** | Take one from the nest on the chest. It squirms in the hand and bites if held too long (a small amount of damage, the Half-Life way). Throw it, or drop it at your feet and let it hunt. |

## Melee

| Weapon | Handling |
| --- | --- |
| **Crowbar** | A physical object with mass. Hits are found by sweeping the crowbar's head (declared in the card) between consecutive commands against hitboxes and the world, on the server; damage scales with the head's speed at contact, with a floor below which nothing happens. Wedge the tip into a breakable vent, grate or crate and lever it: sustained torque applies damage until it gives. |
| **Pistol-whip, butt-stroke** | Any firearm can strike with the same sweep on its body; weaker than the crowbar. |

# Part K — Opposing Force's arsenal

Opposing Force brings the most varied mechanisms in the game, and the census found most of them already rigged. Where no bone is named below, the census has not isolated one yet and the generator or mesh surgery supplies it.

| Weapon | Joints | Handling |
| --- | --- | --- |
| **M40A1 sniper rifle** | bolt `m40a1_bolt` (1.25, in `fire` and `reload2`); magazine `m40a1_clip` (4.81) | The one true two-axis bolt (M-03): lift the handle (hinge), pull back (slide, gated on lifted), push forward, drop. Every shot needs the full cycle. Five-round magazine. Scope with two magnifications (Part H). Shouldering and a braced support hand steady it markedly. |
| **M249 SAW** | `Object05` (17.5) plus parts the generator must classify | Belt-fed: open the top cover (hinge), lay the belt on the feed tray from the box, close the cover, pull the charging handle. The heaviest gun in the game: two hands and shouldering matter most here. |
| **Desert Eagle** | slide and magazine (generator/synthetic) | `magazine_slide` like the pistol, heavier recoil. The secondary-fire laser becomes a switch under the thumb and is drawn from the muzzle. |
| **Displacer** | `spinner` (343.7°) | Charge by holding: the spinner spins up with the haptics. The secondary fire, which teleports the player, gets a comfort fade. No hand reload. |
| **Spore launcher** | `Sphere01` (58.3 in `reload_load`) | Living ammunition: pick a spore from the pouch and push it into the chamber by hand — the model already animates a spore being placed. Primary fires straight; secondary lobs. |
| **Shock roach** | `shock_rifle_body` (19.8) with four telescoping segments | A living weapon that recharges; no hand reload. It wriggles in the hand; the segments move as it fires. Type `none` with moving parts for show. |
| **Combat knife** | — | Melee with the same swept-head method as the crowbar: a short, light blade, fast, stabbing or slashing. |
| **Pipe wrench** | — (census: no drivable part) | Heavy melee. Momentum does the work: a big two-handed swing lands far harder than a flick. The "hold for a heavy swing" secondary becomes simply swinging harder. |
| **Barnacle grapple** | tongue (generator) | Aim with the hand, fire the tongue, hold to be pulled; the pull moves the player with a comfort-limited speed. Letting go drops you. |
| **Penguin** | — | Opposing Force's snark: take, squirm, throw or drop; it explodes. |

**What Opposing Force proves.** Two new joint kinds get exercised here and nowhere in stock Half-Life: the gated two-axis bolt and the belt feed. They are the test that the joint model in Part C is general rather than a set of Half-Life special cases.

# Part L — Malfunctions, fidelity, and keeping it fun

## Four rules that keep full simulation from becoming a chore

1. **A practised player beats the canned reload.** For every weapon, a practised hand reload must be at least 20% faster than Half-Life's own reload animation. This is measured in Part O, and a weapon that fails it is not done.
2. **Hands do motions, never bookkeeping.** No loading single rounds into magazines, no choosing magazines from a list, no counting. The pouch always hands you the right thing, fullest first. An empty pouch closes on nothing with a distinct haptic, never silently.
3. **Every action answers within a frame.** Sound and haptics fire on the predicted event, never on the server's reply.
4. **Nothing forces the player to look away** from the fight for more than a glance. Controls are findable by thumb (Part G); the wrist shows the count.

## Malfunctions (L-01)

Off by default; on in full fidelity, with a rate setting.

| Malfunction | Cause | Clear |
| --- | --- | --- |
| Stovepipe: case caught in the port, action out of battery | a weak grip during recoil (the gun slipped in the hand, Part F) | rack the action |
| Failure to feed: action closed on an empty chamber | an unseated magazine, or a weak grip | tap the magazine, rack |
| Double feed (full only) | rare | drop the magazine, rack twice, reseat |

Deterministic: the engine reports how far the gun slipped in the hand during the shot as one byte in `vrcmd_t`; shared code combines it with the shared random seed. Client and server always agree on whether a jam happened.

## Fidelity settings (L-02)

Per player, in userinfo, like `vr_handload`. In multiplayer the server can set a floor or a ceiling (Part M).

| Mechanic | Arcade | Standard (default) | Full |
| --- | --- | --- | --- |
| Removing a magazine | pull it out | button or pull | button only |
| Chambering after seating a magazine | automatic | slide stop or rack | slide stop or rack |
| Racking a loaded gun | chambered round kept | round ejected | round ejected |
| Grenade fuse | Half-Life's (from pin) | Half-Life's | from spoon release |
| Releasing a weapon | returns to holster | returns to holster | drops |
| Revolver | double action only | both | both |
| Weapon collision with the world | on | on | on |
| Held-weapon mass and lag | reduced | normal | normal |
| Malfunctions | off | off | on |
| Hold reload for a normal reload | yes | yes | yes |

The last row never changes: holding reload for a second always performs an ordinary reload on any weapon. It is the escape hatch that keeps a broken card from ever stranding a player.

# Part M — Multiplayer

**The rules are the same online.** Because every game-changing decision is made by shared code on the server and predicted by the same code on the client, nothing in Parts C–L needs a multiplayer variant. What differs is settings and what other players see.

| Topic | Online behaviour |
| --- | --- |
| Authority | Server runs `vr_weapon` for every hand-loading player; clients predict with the same code; mechanism and round state ride in weapon data under `NET_EXT_VR` |
| Aim | The simulated gun pose is client-reported, like view angles always have been. The server checks it is plausible: the gun within arm's reach of the body, not moving faster than a hand can, and the muzzle reachable from the eye (Part F) |
| Melee | Server sweeps the reported crowbar or knife poses between commands; speeds clamped to what a hand can do |
| Grabs | Server validates reach before attaching anything to a hand (Part I) |
| Dropped magazines, rounds, weapons | Ordinary server entities; everyone sees them and anyone can pick them up — including an opponent's dropped magazine |
| Spent cases | Drawn by each client from the existing weapon events; never networked |
| Mechanism sounds | Rack, magazine out and in, and cylinder sounds go to other players through the existing client event system, flagged so the shooter's own client (which already played them on prediction) skips them |
| Seeing other VR players | Head, both hands, gun pose, the eight joint values and the weapon ID are relayed in the player's entity state at snapshot rate. Each client draws that player's weapon model posed in the world, with hands and IK arms (Platform plan, Part G) |
| Desktop players | Keep stock weapons and stock reloading; nothing here applies without `vr_handload` |
| Fairness | Server cvars set a floor and ceiling on fidelity (for example, no arcade chambering in competitive deathmatch, or malfunctions forced off), and whether VR players may use the hold-to-reload escape. Weapon collision stays on for everyone because it removes an advantage rather than adding one |
| Hitboxes | A VR player's hitbox follows the solved body, so leaning and crouching count the same for both kinds of player |

**Bandwidth.** `vrcmd_t` v2 is typically 10–30 bytes per command after delta encoding (both hands moving), roughly 1–3 KB/s upstream at 60–90 commands a second. The relay for each VR player is around 30–40 bytes per snapshot. Both are small next to what GoldSrc already sends.

**Co-op.** Nothing weapon-specific; the co-op engine limits (saves, level carry-over) are in the Platform plan. One addition: weapons and magazines dropped in the world should carry across level changes with the player's inventory when co-op carry-over is implemented.

# Part N — Milestones

**Priority.** The owner has made this the top priority (29 September). It replaces step 2 of the Platform plan and takes precedence over the campaign playthrough. The step-0 fixes and the 64-bit switch still come first, because everything here is built on them.

**Starting point.** Already built by the coder: `vrcmd_t` v1 filled per command, the `GetVRWeaponAPI` export, `vr_weapon` v0 (event derivation from part values) wired into the weapons, and a headless test with 11 passing cases. Not yet seen: the block crossing a network, and any of it in a headset.

| ID | Milestone | Contents | Needs | Done when |
| --- | --- | --- | --- | --- |
| W0 | Prove what exists | Two-instance network test of `vrcmd_t` v1; headset session of `vr_weapon` v0 against the feel checklist | step 0, 64-bit | Block seen on the wire; checklist green on shotgun, pistol, revolver |
| W1 | Mechanism core | Fixed-point joint simulator, catches, detents, crossings (C); feed path and rounds (D); `vrcmd_t` v2 hands; renderer reads predicted joint values; card v2 with fingerprints; prediction state in weapon data | W0 | Pistol, revolver, shotgun, crossbow and RPG fully hand-operated in SD and HD; prediction-mismatch counter reads zero over a recorded session; one headless test per feed rule |
| W2 | Content | Card generator; mesh surgery; census preview mode | W1 | MP5 with charging handle and hand-loaded grenade launcher; synthetic slide stop, magazine release, hammer, cylinder latch, loading gate; generator run over all 106 viewmodels with every card reviewed |
| W3 | Hands and controls | Grip solver, trigger discipline, thumb controls, trigger joint and sear, single/double action, haptic patterns | W1 | Every control findable by thumb without looking; revolver single and double action; haptic table implemented |
| W4 | Physics | Physics module on client and server; held weapon as rigid body; hand springs; recoil; shouldering; weapon collision; muzzle reachability; physical casings; racking against surfaces | can start beside W1 | Pistol lag under 2 mm at rest; cannot fire through a wall locally or on a server; comfort limits hold |
| W5 | World and body | Server physics objects; grabbing; dropping and placing weapons and magazines; body holsters; pouch; wrist display; dual wield | W4 | Drop a half-empty magazine, walk away, come back, pick it up, load it, fire the remaining rounds |
| W6 | Sights | Scope view pass and magnification; field-of-view zoom suppressed; reticles at infinity; hand-steered RPG laser | renderer aux-view pass | Crossbow and M40A1 scopes usable at both magnifications, with no view zoom |
| W7 | Half-Life specials | Grenades with pin and spoon, satchel radio, placed tripmines, snark, Gauss charge and jump, Egon, hornet gun, crowbar sweep and prying, pistol-whip | W4, W5 | Every Half-Life weapon in Part J works as written |
| W8 | Opposing Force arsenal | Everything in Part K, including the two-axis bolt and the belt feed | W1–W6 | Every Opposing Force weapon in Part K works as written |
| W9 | Options and online | Malfunctions; fidelity presets; server floor and ceiling; relaying other players' weapons | W1–W8, Platform netplay step | Two-instance session: a VR player sees another's slide lock back and magazine hit the floor |

**Parallel tracks.** One coder can run W4 (physics module) and W6's renderer pass alongside W1–W3, because they touch different code. W1 is the critical path: nothing gameplay-facing moves until the simulator exists.

# Part O — Testing, feel and risks

## Testing

The owner's headset time goes to feel. Everything else is checked by machine.

1. **Headless simulator tests.** The existing harness (`vr_weapon_test.cpp`, 11 cases) grows to one case per feed rule, catch, control, fidelity setting and malfunction. Each is a scripted list of `vrcmd_t` commands and an assertion about rounds and joint states.
2. **Cross-build determinism.** Run the same recorded command stream through the simulator compiled as part of the client DLL and as part of the server DLL, in 32- and 64-bit, and compare a hash of the full state after every command. Any difference fails the build. This is what the fixed-point choice in Part C buys; this test is what proves it.
3. **Recorded sessions.** Record `vrcmd_t` streams alongside the pose recordings from the Platform plan's harness. Replaying a stream through the simulator and diffing the event log catches regressions without a headset.
4. **Prediction-mismatch counter.** The client logs a hash of its predicted weapon state per command next to the server's confirmed one. Must read zero on local play and on a two-instance session with simulated lag and packet loss.
5. **Reload benchmark.** For every weapon, time from starting a reload to the next shot, by a practised player, against Half-Life's canned reload. Rule 1 of Part L (20% faster) is checked here and the numbers are kept in a table in the repository.
6. **Physics metrics.** Logged per session: held-weapon lag at rest and during fast motion, recoil recovery time, collision push-back events.
7. **Census regression.** The generator runs over all 106 viewmodels on every change to it; generated cards are diffed against the last reviewed set.

## The feel checklist grows

`PCVR_FEEL_CHECKLIST.md` keeps every current item and gains one section per Part, written before that Part is built: comfort limits (F), findability of controls (G), no view zoom (H), the four rules (L). An item is ticked only in the headset.

## Risks

| Risk | Mitigation |
| --- | --- |
| Feel regresses during the rewrite | The current path stays behind a cvar until W1 passes the checklist on every weapon it covers; only then is it deleted |
| Held-weapon physics feels laggy or floaty, or causes discomfort | Comfort caps (Part F); a "pinned" setting that restores today's direct attachment for players who want it |
| Client and server DLLs disagree numerically | Fixed-point simulator plus the cross-build hash test |
| Mesh surgery corrupts a model | Only client viewmodels; validator refuses bad selections; preview mode in the census tool; the unmodified model is kept as fallback |
| The physics library complicates the C engine build | A small C wrapper; built and tested on both 32- and 64-bit from the first day |
| Scope view costs too much frame time | Capped texture size; single-pass stereo (Platform plan, R0) first |
| The generator fails on mod weapons | Guessed cards never drive catches they are unsure of; fallback to the ordinary reload; hold-to-reload always works |
| Grabbing waits for the server | The client shows a grab immediately and the server confirms; a refused grab (rare) snaps back |
| One tester | Every item above that can run without a headset does |

## Open questions for the owner

- [ ] Default fidelity preset: **standard** proposed.
- [ ] Malfunction default rate when full fidelity is chosen.
- [ ] Grenade fuse in standard fidelity: Half-Life's (from pin) or real (from spoon)?
- [ ] Released weapons in standard: return to holster (proposed) or drop?
- [ ] Your IQM hands: the grip solver in W3 should target the hands you will ship. If the IQM hands are coming, they should land before or with W3.
