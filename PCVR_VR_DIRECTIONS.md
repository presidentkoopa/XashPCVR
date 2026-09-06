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
