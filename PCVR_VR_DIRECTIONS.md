# Directions for VR in this engine

Written after the weapon-part work landed (`ce7b5d14`), as a standing reference for
what this engine can be made to do and why. It is a design document, not a plan of
record — nothing here is committed to.

## The principle

Every hard failure in the weapon work had the same shape:

> **The engine knew something and told nobody, so the VR layer inferred it.**

Bone positions existed every frame; the VR layer inferred reach radii. Travel
distances were sitting in the models; constants were tuned by feel instead. Which
part the player meant was decidable from where their hand was; it was guessed from
gesture shape.

The fix was never a better inference. It was making the engine **say what it already
knew**. `R_StudioApplyHandAction` now publishes where every drivable part is, which
way it travels, and how far — and the guessing stopped.

Apply that lens to everything below. When a VR feature feels like it needs tuning
constants, the real question is usually *what does the engine already know that it
isn't saying?*

## Why general beats specific here, by a factor of a thousand

Anything built at engine level lands on **every GoldSrc mod at once** — roughly 1200
of them.

- **Lambda1VR** forks the game code per mod. It supports four.
- **HLVR** is one game.
- We are the only one of the three where a capability added once appears in
  everything ever made for the engine.

That is the strategic asymmetry this project has, and it argues for a specific bias:
prefer capabilities **derived from data the engine already holds** over capabilities
that need someone to author something. Authored data exists for stock Half-Life and
nothing else. Derived data exists everywhere.

## Half-Life already models hand-driven objects

`CMomentaryRotButton` — the valve wheels and cranks throughout Half-Life:

```c
pev->ideal_yaw = AxisDelta( spawnflags, pev->angles, m_start ) / m_flMoveDistance;
```

A **0-to-1 position along a travel**, with `m_start` and `m_end` as its rest and
extent poses. Structurally identical to a weapon slide. Valve built the data model
for hand-driven world objects in 1998 and then only ever let the player hold a key
down to move them.

The part driver does not care that it is pointed at a weapon.

## Near-term — mostly already written

**Grab the world, not just the gun.** Point the part driver at
`momentary_rot_button`, `momentary_door`, `func_rotating`. Reach out, take hold of a
valve wheel, turn it as far as you turn it, stop where you stop. Half-Life's valve
puzzles and reactor sequences become physical. Probably the largest immersion change
available per unit of work remaining.

**Touch instead of aim.** `PlayerUse` is a sphere search plus a dot product from the
eye. Every entity already carries **hitboxes** — per-bone volumes — that nothing in
VR reads. One general "what is my hand inside" service makes every button, lever and
handle in every mod something you press rather than look at. The same service later
answers "am I touching that scientist."

## Bigger

**Solve a body.** Head and two hands is enough for plausible shoulders, elbows and
torso. The point is not a cosmetic body — it is that **holsters stop being tuned
offsets**. Pouch and holster positions were guessed repeatedly during the weapon
work; a shoulder solve derives them from the player's own proportions. Leaning,
peeking and two-handed shouldering fall out of the same solve.

**Melee that knows the weapon's shape.** Currently a hand-speed threshold: swing fast
enough, anywhere, and you hit. The crowbar's head bone has a known position. Real
contact point, real arc, and the ability to genuinely miss *because the crowbar is
short*. The difference between a gesture and a tool.

**NPCs as physical things.** Bones and hitboxes exist on every monster. Pull a
scientist along by the arm. Shove a grunt. Raise a forearm to block a headcrab.
Damage located by where the hand actually was.

**Props.** Grab and throw anything. The throw half is already solved — launch
velocity comes from hand speed as of `8131983f`.

**Mantling.** The ladder work generalises. Climbing is "find a surface, decompose
intended motion against it." A ledge is the same problem with a different surface
test: out of water, over crates, onto pipes.

**Let other players see it.** `pev->controller[]` is already part of the networked
entity state. Part positions could ride it — a slide locked back, visible to everyone
on the server, with no protocol change.

## Honest limits

- **GoldSrc has no rigid-body solver.** "Grab a crate and it swings under its own
  weight" is not something to enable, it is something to write.
- **Content quality varies wildly.** A mod with a badly boned weapon must degrade
  gracefully, never break. Everything here needs to fail soft and fall back to the
  existing behaviour.
- **Authored data is a trap.** Any design that needs per-mod tagging works on one
  game and nothing else.

## Suggested order

1. **World parts** — reuses the weapon-part driver almost entirely, and it is the
   moment the whole game becomes touchable rather than just the gun.
2. **Body solve** — retires a whole category of tuned constants, and tuned constants
   are what cost the most time in the weapon work.
3. Everything else, by feel.

---

# A VR-forward Xash

## The reframe

Today's engine is a **flatscreen engine with VR substituted into it**. Every VR
feature works by intercepting something the engine does for a monitor player and
swapping in hand data — `view_ofs` around `PostThink`, `cmd.viewangles` around
`PM_Move`, buttons synthesised from gestures. That pattern has carried the project
a long way and it is why mod compatibility survived.

But it is also why the same wall keeps appearing. The engine's defaults are all
flatscreen defaults: a weapon is a viewmodel painted over the world, aim is a view
angle, "use" is a trace down the nose, interaction is a keypress, the HUD is on
the screen, and the player is a cylinder with one eye.

**A VR-forward Xash inverts the defaults.** The player is a head and two hands in
a room. The flatscreen player becomes the special case — a head with no hands,
aiming along its nose — rather than the other way around.

Concretely, the inversions:

| Flatscreen default | VR-forward default |
| --- | --- |
| Weapon is a viewmodel | Weapon is a world object in a hand |
| Aim is a view angle | Aim is a hand pose |
| Use is a trace from the eye | Use is a hand touching something |
| Interaction is a keypress | Interaction is a grab |
| HUD is on the screen | HUD is on the body |
| Recoil kicks the camera | Recoil kicks the weapon |
| Weapon parts play animations | Weapon parts are moved by hands |

Only the last one is done. It took a week and it was worth it.

**The game DLL interface does not change.** That is the whole trick, and it is
already proven: the engine can be VR-native internally while still speaking
flatscreen to the mod. Substitute into engine-owned memory the DLL reads, restore
afterwards. 1200 mods never find out.

## Delete the viewmodel

GoldSrc does not draw your weapon in the world. It draws a **viewmodel** — a
separate pass with its own FOV, painted over everything at the end. It does not
occlude, does not collide, casts no shadow, is not lit by the room, and no other
player can see it. On a monitor that is a clever cheat. In VR it is a lie you can
feel.

Make the weapon a real world entity parented to the hand and every one of those
breaks fixes at once: true scale, correct occlusion, real shadows, lit by the
actual room, visible to other players.

And then the consequence worth having: **the weapon can collide.** Push the barrel
into a wall and it is pushed back. You cannot put it through a door and fire.
That is not a feature to add; it falls out of the weapon becoming real.

Neither Lambda1VR nor HLVR can do this. They do not own their renderers.

## Delete weapon animations

The logical end of the part work, and not a joke.

If every moving part is driven by the hand, and the weapon's position IS the hand,
what is the animation system still for? Idle, fire, reload, draw, holster — each
is a canned performance of something the player now does themselves.

The endpoint: the weapon's pose is the hands. Its parts are where they were left.
Recoil is an impulse. **No canned animation anywhere in the weapon pipeline** — and
every class of bug fought during the part work stops existing rather than being
worked around: cycles that end where they start, recoil spikes inside the travel,
animator noise on driven bones.

## Fingers

Every weapon viewmodel carries **30 finger bones** — five fingers, three joints,
both hands — rigged by Valve, shipped in every model, driven by nothing.

Grip and trigger are analog. Curl by grip pressure. Wrap the fingers around
whatever part is actually held. And the detail VR players care about more than
almost anything: **trigger discipline** — the index finger rests alongside the
guard until the trigger is actually pulled. The rig is already there.

## Recoil as force, weight as feel

Stop kicking the camera. It is both nauseating and false — the gun moves, not the
eyes.

Apply an impulse to the weapon and let it settle. Two hands: less. Braced: less.
A shotgun fired one-handed nearly leaves the grip.

The same system carries **weight**: the weapon lags the hand by its mass, which can
be derived from the model's own bounds rather than authored, so it works on mods
nobody tuned.

## Real acoustics

The engine owns the sound system and the BSP, and GoldSrc has essentially no
spatial audio — no occlusion, no reverb, no head-relative positioning.

Room size from leaf data drives reverb. A wall between the player and a gunshot
filters it rather than only attenuating. Sound is positioned from the **actual
head**, which today it is not.

Audio is the cheapest presence multiplier in VR, it is pure engine work, and it
lands on every mod without any of them knowing.

## Find the parts automatically

The one that changes what the project is.

Parts are named per model in a cvar today, which makes this a Half-Life feature.
Auto-discovery makes it a **GoldSrc** feature — every mod, no configuration.

An earlier attempt failed, but it failed on weak signals ("which bone moved most",
which picked a fingertip). The signals available now are much sharper: a part is a
bone that owns a coherent share of the mesh, travels along a single consistent axis
during fire or reload, and returns to rest. Those three together are a far better
filter than any one of them.

## Synthetic travel

Some models do not animate what the hand wants to move. Where that happens, the
travel can be **declared** rather than derived — this bone, this axis, this many
degrees — so a part exists even when the animator never made one.

This is the answer to "the models were not made for this," and it is only available
because the engine is ours to change.

## Haptic detents

A part's travel is known exactly, so the events along it are known too: the slide
passing the catch, the cylinder clicking into each chamber, the pin clearing the
grenade, the bolt stripping a round.

Not rumble-on-fire. Mechanical texture generated from the model's own geometry.

## The HUD goes on the arm

The HEV suit is canonically a wearable computer, so put it on the arm — health and
ammo read by looking. `v_hand_hevsuit` already exists. Then the floating HUD can be
switched off entirely and the last non-diegetic thing in view is gone.

## Make it a platform

Expose the part table as **data a mod ships** — a small file beside the model —
rather than engine cvars.

Then a mod author VR-ifies their own mod without touching engine code, and this
stops being a way to play Half-Life in VR and becomes the thing people build GoldSrc
VR mods on. That is how it outlives its authors.
