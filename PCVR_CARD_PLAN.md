# VR reloading: the decided course

Status: PROPOSAL v2, **partly superseded**. Section 5 is replaced — see the note there —
and open decision 1 is answered. Sections 1-4 and 6 still stand, and the platform brief
builds on them. Supersedes v1 of this file.

Two independent studies arrived at the same route:

- **This plan**, drawn from RS_VR_Reload — the owner's hand-driven reload system for
  DoomXR, 17,000 lines and working. Digests of it: `rsvr/CODE_DIGEST.md` (from its source)
  and `rsvr/DOCS_DIGEST.md` (from its documents, including 42 tuned "feel rules"), in the
  session scratchpad.
- **`XASH_MANUAL_RELOAD.md`** (Desktop/REVIEW), a study of what GoldSrc can and cannot do,
  which ranked every alternative route and funded only this one.

They agree on the destination. Where they differ is sequencing, and this document settles
it.

---

## 1. The route, and the four that are ruled out

**Chosen: renderer-side bone drive, with the mechanism declared in data.** The parts are
already separate bones in stock Valve content — the pump, the pistol slide, the MP5
magazine, the revolver cylinder and its speedloader. The renderer measures each part's
travel out of the model's own sequences and publishes where it is; the hand reaches it and
moves it. No content work, no recompiled models, no network traffic for the visual half.

Ruled out, with the evidence, so nobody re-opens them:

- **Bone controllers.** Zero declared across all seventeen Half-Life viewmodels. Four
  usable per model (index 4 is hardcoded as the mouth), 8-bit, and declared in the QC at
  compile time. Every weapon in every mod would need decompiling to get a worse version of
  what we already run. (The SDK header says 8 controllers, our engine's says 32; the
  networked state is `byte controller[4]`, so the usable count is four either way.)
- **A magazine as an entity while it is in the gun.** `MOVETYPE_FOLLOW` copies origin and
  angles only; GoldSrc has no bone parenting. A dropped magazine can be an entity *after*
  it leaves — which is what we already do.
- **Re-authored models with per-part sequences.** Buys nothing the measurement does not
  already extract, and forfeits every mod nobody re-authors.
- **Bodygroups as the motion lever.** `body` is networked and works end to end, but it is a
  switch, not a motion: present or absent, never half out. Half-Life's viewmodels have
  already spent their two or three bodyparts on hand and skin variants. Keep it in mind as
  a fallback for visibility only.

## 2. What changed after reading both

**v1 said "renderer-side drive from day one". That was wrong, and here is why.**

RS_VR_Reload needs its renderer to hold the part against the hand because GZDoom's playsim
runs at 35 Hz — script's estimate can be most of 30 ms stale, which is visible and is the
root of half its feel rules. **Our VR layer already runs per rendered frame**, inside
`VR_BeginFrame`, so our equivalent staleness is a single frame. That is a far smaller debt
than theirs, and it does not justify paying the largest engine cost first.

What *is* ours, and is not about where the drive is computed: ownership of a part flips
through a sentinel value (`-1` means "not ours"), and that flip is the source of the
snapping, the parts that freeze at rest, and the dissolve that leaked onto the next
weapon. One state per part, with pose and drive agreeing at every value, fixes those
without moving anything into the renderer.

So: **data first, mechanism second, fidelity last and only if measured.**

## 3. The measurement rule

The review treats "measured out of the model's own sequences" as the win, and it is.
RS_VR_Reload documents the opposite lesson in blood: frames lie — its M4A3's receiver
drifts 24 units mid-animation, and a pin measured on the wrong mesh was wrong for four
days.

Both are right, and the card format resolves them:

1. **Measure to seed.** The derived travel is the default and needs no authoring.
2. **The card overrides.** Any measured number can be stated instead.
3. **The validator flags a suspicious measurement** at load: a travel of a fraction of a
   unit, an extent taken from a holster sequence, a peak one frame wide. The pistol's
   one-frame recoil spike cost a week of headset runs; it becomes a warning line.

## 4. Phases

**Phase 0 — ground.**
- Tag the current commit; keep a zip of the deployed build.
- Write down every behaviour today's code gets right, as an acceptance checklist. About
  fifteen of RS_VR_Reload's feel rules apply to us directly.
- Census the models (Half-Life SD and HD, Opposing Force, Blue Shift; MMod for reference).

**Phase 1 — data (the biggest win, no risk to feel).**
- Card format: parts (bone, DOF, grab volume, hand seat), stores, verbs, archetypes,
  `base =` inheritance.
- Parser and validator: an unknown key refuses that one weapon and names the line; a bone
  the model lacks refuses the card and lists the names it has; a suspicious measurement
  warns.
- Per-gamedir file, so Opposing Force's rigs are Opposing Force's file. This replaces one
  cvar whose per-weapon entry truncates at 255 characters.
- A guessed card for uncarded mods, labelled as a guess in the log.
- Cards feed the existing code. No behaviour change. Ends the guessing.

**Phase 2 — mechanism.**
- Verb interpreter: `cycle`, `open`, `swap`, `load`, `eject`, plus `dof2` for a bolt
  action. The load-bearing key is `return = spring | hand | stay`.
- Weapon code moves out of `vr_openxr.c` (11,486 lines) into its own file.
- A cvar switches old versus cards while both exist, so "they feel the same" is tested in a
  headset rather than trusted — then deleted.

**Phase 3 — fidelity, measured first.**
- One ownership state per part; pose and drive agree at every value.
- Then measure the frame lag in a headset. Only if it is felt: move the drive into the
  renderer with drawn-value read-back, and bump `REF_API_VERSION` (the current VR fields
  were added without bumping it).

## 5. The ammunition half — SUPERSEDED

> This section argued that the mod owns the ammunition, that hand loading therefore
> cannot empty a gun on a mod we have not rebuilt, and that this limitation is what
> makes the design netplay-safe. **The premise no longer holds, and it was doing the
> load-bearing work here.** Replaced by the architecture in the platform brief; kept
> below, struck through, because the reasoning is worth being able to re-read.

What changed is 64-bit. A 64-bit engine cannot load a closed 32-bit game DLL at all, so
nearly every mod that actually runs is now one whose DLL we compile from
`hlsdk-portable`. "The mod owns the ammunition" described a world where we could not
touch the game code. We can.

So weapon state moves into shared code (`vr_weapon`) compiled into both `hl.dll` and
`client.dll`, run by the server and by client prediction on the same command. That is
netplay-safe for a better reason than the old one: not because the engine stays out of
the ammunition, but because both copies run identical code on identical input. The
engine's job shrinks to reporting what the hands did.

Three consequences for what this document says elsewhere:

- The three impulses are a transport that loses events — one impulse per command means
  two in a frame lose one, and prediction never sees them at all. They are replaced by a
  hand **state** block on each command. State replayed twice gives the same answer twice;
  an event replayed twice fires twice.
- `VR_ActionBlocked`, which suppresses `IN_ATTACK` client-side, is for game DLLs that do
  not speak the new interface. Where they do, the weapon refuses to fire because its
  chamber is empty, and server and prediction agree by construction.
- The engine stops inferring weapon state from the `CurWeapon` HUD message, which arrives
  a round trip after the shot.

The fallback path — parts move visually, `IN_RELOAD` pulsed per insert, engine blocks the
trigger — remains, and must keep working. That is what a closed mod or Diffusion gets.

RS_VR_Reload's finding that every carded shot desyncs still stands as a warning; it just
has a different answer. Its script owned the ammunition and had no second copy to agree
with. Ours has one, and they are the same code.

~~*Original text.* Three impulses (210 action worked, 211 magazine out, 212 cylinder
dumped) behind the per-player `vr_handload` userinfo gate, mirrored in prediction. Keep it
this size. The limit, stated rather than engineered around: on a mod we have not rebuilt,
the hands move the parts but pulling a magazine cannot empty the gun. The mod's DLL owns
the ammunition. That same limit is why this is netplay-safe. A card drives bone poses of
the local viewmodel and suppresses input; it adds no message, no entity and no protocol
change, and two VR clients share no state.~~

## 6. Open decisions

1. ~~Card file location — proposed `<gamedir>/vr/weapons.txt`, with a built-in default for
   Half-Life.~~ **Answered by the census.** A card cannot be keyed by gamedir alone:
   `valve` holds two different rigs, because `valve_hd` is an overlay mounted over it when
   HD models are on, and the two need entirely different bone names. A card binds to the
   model it was measured from, not to the directory it was found in.
2. v1 scope: cut spinning barrels, flipping parts, fill meters, second barrels and
   throwables; keep the five verbs and `dof2`.
