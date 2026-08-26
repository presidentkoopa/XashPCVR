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
engine loads those verbatim — which is the whole reason this fork stays 32-bit. FWGS's own
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

## Architecture: 32-bit and 64-bit, both

`COM_GenerateServerLibraryPath()` uses a mod's declared DLL filename **verbatim**
only on 32-bit Windows. Every other architecture rewrites it with an `_amd64`
suffix. Legacy GoldSrc mods ship 32-bit DLLs exclusively, so a 64-bit engine
cannot load any of them — which is why **32-bit is the default** and is the build
that plays the ~1200-mod catalogue.

But modern standalone Xash titles ship **amd64-only** binaries and are unreachable
from a 32-bit engine. So the fork builds both. Same VR layer either side; it is
architecture-agnostic C.

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

* **Nothing is added to the network protocol.** Vanilla clients still connect.
* **Every VR substitution is scoped to the local player** via `NET_IsLocalAddress`, so on a
  listen server remote players are untouched by it.
* **A dedicated-server build contains no VR code at all** — it is all behind `#if !XASH_DEDICATED`.

Two honest limits:

**Hosting works; joining does not.** The server-side substitutions only apply to the local
player, so a VR player who joins someone else's server reverts to firing from the eye. Fixing
that means sending controller pose in `usercmd_t`, whose four `reserved` fields are already
spent — so it is a protocol break that would lock out unmodified clients, not a feature.

**Co-op content is the real blocker, not the engine.** Vanilla Half-Life has no co-op and Sven
Co-op ships its own engine. The promising route needs no game DLL at all: the engine forces
`deathmatch 0` when `coop` is set, which selects *singleplayer* game rules with several clients —
and singleplayer rules always spawn monsters. Savegames and cross-level entity carry-over are
disabled whenever `maxclients > 1`, so a co-op campaign has neither.

Detail in [`PCVR_LOG.md`](PCVR_LOG.md) → **FINDING 018**.

## VR-to-VR crossplay

Planned, not built. The substitutions that make weapons fire from the controller are scoped to
the **local** player (`NET_IsLocalAddress`), so today only the VR player hosting a listen server
gets VR-correct behaviour — anyone who joins, VR or not, reverts to eye-origin aim.

Making a joining VR player's shots leave their own controller means the server needs their
controller pose, and `usercmd_t` has no room left for it — its four `reserved` fields are already
spent (see Online and co-op above). So this is a protocol change, gated behind a version bump so
vanilla and older PCVR clients still connect to an unmodified server: the substitution moves from
`sv_pmove.c`'s local-only branch to reading pose out of the usercmd for any client that negotiates
the extension, host or joiner alike.
