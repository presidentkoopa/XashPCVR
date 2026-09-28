# Xash3D FWGS — PCVR fork

A room-scale PCVR port of Xash3D FWGS (OpenXR), built to play **arbitrary GoldSrc mods with
their own custom content** in VR — not just one hand-patched mod.

**The big deal: weapons fire from your controller with a completely unmodified game DLL.**

Full write-up: [`PCVR_LOG.md`](PCVR_LOG.md) → **FINDING 017**.

Other VR features: stereo OpenXR rendering, head/hand tracking, hand + weapon models,
two-handed weapon stabilisation (geometry-based grip), laser sight, grenade trajectory arc,
swing-to-hit melee, off-hand flashlight, haptics, stick locomotion with snap turn, in-headset
HUD, and a desktop mirror.

## Mod compatibility

| **Opposing Force** | Loads its own shipped `opfor.dll` — no rebuild needed |
| **Blue Shift** | Playable, via rebuilt DLLs at `bshift/` [(why)](#mod-compatibility) — the Steam release ships vgui2, which Xash cannot load |

Most legacy mods need nothing done to them at all. They ship 32-bit game DLLs, and a 32-bit
engine loads those verbatim — which is why the 32-bit build is kept as a fallback rather than
retired. A mod whose source is available is instead rebuilt as a 64-bit DLL we compile. FWGS's own
[`supported-mod-list.md`](Documentation/supported-mod-list.md) runs to ~1200 entries and is the
real compatibility list.

## Mods planned

Full target list, tiered by what's needed to run each one, is
[`PCVR_MOD_TARGETS.md`](PCVR_MOD_TARGETS.md). Short version:

* **Working now** — Half-Life, Opposing Force, Blue Shift.
* **Game DLL built** — They Hunger, Half-Life: Decay,
  Half-Life: Echoes, Afraid of Monsters: Director's Cut, Poke 646 (+ Vendetta), Half-Life:
  Induction, Delta Particles, Deathmatch Classic, Absolute Redemption, Half-Life: Urbicide.
* **Next up** — Azure Sheep, Counter-Life, Half-Life: Insecure, Half-Life: Top-Down, Half-Life:
  Visitors, Half-Rats: Parasomnia, Half-Life: Field Intensity, They Hunger-family mods
  (USS Darkstar, The Gate, TWHL Tower, Halfquake).
* **Ruled out** — anything requiring vgui2 (Counter-Strike 1.6, Condition Zero, Day of Defeat),
  Sven Co-op (own engine), Half-Life: Extended (long-term, once released).

## Architecture: 64-bit primary, 32-bit fallback

**64-bit is the primary build.** It is the one that gets tested, and the one the
flagship campaigns — Half-Life, Opposing Force, Blue Shift — are being taken
through. The 32-bit configuration keeps compiling as a fallback and is built
alongside on every change so it does not rot, but it is no longer the default.

That reverses what this document said until September 2026, and the reason is
worth keeping. `COM_GenerateServerLibraryPath()` uses a mod's declared DLL
filename **verbatim** only on 32-bit Windows; every other architecture rewrites
it with an `_amd64` suffix. Legacy GoldSrc mods ship 32-bit DLLs exclusively, so
a 64-bit engine cannot load a closed one at all. What changed is not that
limitation but what we do about it: open mods are rebuilt from `hlsdk-portable`
as 64-bit DLLs we compile ourselves, which is also what lets shared weapon code
run inside them. Closed 32-bit mods are the price, and they are why the 32-bit
fallback stays alive.

Modern standalone Xash titles ship **amd64-only** binaries and were unreachable
from a 32-bit engine, which is the other half of the reason. Same VR layer
either side; it is architecture-agnostic C.

**64-bit status:** the engine builds, loads 64-bit game DLLs (`hl_amd64.dll`,
`client_amd64.dll`), spawns a map, and the VR layer reaches the OpenXR runtime and
creates an instance. Stereo rendering itself is not yet verified in a headset.

Getting there needed a 64-bit OpenXR loader, built from the SDK into its own
`build64` so the 32-bit one is untouched — `engine/wscript` already probes per
architecture, so no build-system change was required. It also surfaced a real bug:
`vr_openxr.c` was guarded by `#if XASH_WIN32 && !XASH_DEDICATED` but not by
`XASH_OPENXR`, and the source glob is unconditional, so **any** build configured
without a loader failed on the include rather than falling back to flatscreen.

Plan and detail: [`PCVR_64BIT_PLAN.md`](PCVR_64BIT_PLAN.md).

## Online and co-op

The VR work was built to survive multiplayer rather than assume single player, so:

* **Protocol changes between our own builds are allowed.** Each goes behind a
  capability bit in the existing `ext` handshake, and a mismatched build is
  refused at connect with a message naming both versions — never allowed to
  connect and quietly disagree. Compatibility with stock GoldSrc and vanilla
  Xash is kept where it costs nothing, and is no longer a requirement. What is
  described below was built without a protocol break; that is a property of
  that particular change, not a rule binding the next one.
* **Every VR substitution is scoped to the local player** via `NET_IsLocalAddress`, so on a
  listen server remote players are untouched by it.
* **A dedicated-server build contains no VR code at all** — it is all behind `#if !XASH_DEDICATED`.

One honest limit — and one that used to be listed here and no longer is:

**Hosting and joining are written, and have never been run.** The code is there, and the
reasoning below holds, but no session has ever been played between two machines — there is one
PC and one tester. Treat it as untested until a two-client test on localhost passes. This
previously read "both work", which the code justified and the testing did not.

It was also once listed as a hard limit, on the grounds that `usercmd_t`'s four `reserved`
fields were spent and carrying pose would be a protocol break. That part was wrong — see
**VR-to-VR crossplay** below.

**Co-op content is the real blocker, not the engine.** Vanilla Half-Life has no co-op and Sven
Co-op ships its own engine. The promising route needs no game DLL at all: the engine forces
`deathmatch 0` when `coop` is set, which selects *singleplayer* game rules with several clients —
and singleplayer rules always spawn monsters.

Two engine limits apply regardless, and they are worse than previously recorded here. Saving is
blocked **outright** whenever `maxclients != 1` (`sv_save.c` refuses with "Can't save multiplayer
games") — not merely the load path — and cross-level entity carry-over is disabled when
`maxclients > 1`. A co-op campaign would therefore be one unbroken sitting with inventory lost at
every level boundary. Neither is VR work; both are engine surgery.

Detail in [`PCVR_LOG.md`](PCVR_LOG.md) → **FINDING 018**.

## VR-to-VR crossplay

**Written, never run.** The mechanism below is implemented and costs no protocol break, but it
has never been exercised between two clients. A joining VR player's shots *should* leave their
own muzzle; nobody has watched it happen.

The earlier plan assumed one was unavoidable, because `usercmd_t`'s four `reserved` fields looked
spent. They are not. `net_encode.c` binds them to `impact_index` / `impact_position`, and that
binding is the *entire* set of references in the tree — nothing reads them, nothing writes them,
nothing validates them. They are a name-to-offset mapping so the delta parser can match GoldSrc's
`delta.lst` vocabulary, and no more.

Better still, `impact_position` is already exactly the carrier this needs. `delta.lst` declares it
`DT_SIGNED | DT_FLOAT`, 16 bits, divisor 8 — **±4095.875 units at 0.125-unit (3.2 mm) resolution**,
precisely the GoldSrc map extent, transmitted every frame and always containing zero. Direction
needed nothing at all: `cmd->viewangles` already carries the weapon angles.

So the change is additive. `usercmd_t` does not change size, its `STATIC_CHECK_SIZEOF` is untouched,
the delta table is untouched, `PROTOCOL_VERSION` stays 49, and `sv_client.c`'s version check never
fires. Capability is negotiated through the **existing** `ext` handshake, which already has
intersection semantics — the server acknowledges only bits it understands and echoes the result —
so every direction degrades on its own:

| | |
|---|---|
| VR client → vanilla server | server ands `NET_EXT_VRPOSE` away; client falls back to eye-origin |
| vanilla client → VR server | never sets the bit; server never reads a pose |
| old PCVR client → new PCVR server | same mechanism, one bit per capability revision |

Guarded against mods that genuinely use those "reserved for modders" fields: a six-bit sentinel
accompanies the pose, and the client yields entirely (warning once) if the mod's own `CL_CreateMove`
already wrote them — the mod's data wins.

One consequence worth calling out: the read side needs no VR code at all — three floats and a
`VectorSubtract` — so it sits outside `#if !XASH_DEDICATED`. **A dedicated server can host
VR-correct players with no OpenXR, no headset, and no client VR layer compiled in.**
