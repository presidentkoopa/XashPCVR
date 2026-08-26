# Xash3D FWGS — PCVR fork

A room-scale PCVR port of Xash3D FWGS (OpenXR), built to play **arbitrary GoldSrc mods with
their own custom content** in VR — not just one hand-patched mod.

**The headline result: weapons fire from your controller with a completely unmodified game DLL.**

Every stock Half-Life weapon traces its shot from `CBasePlayer::GetGunPosition()`, which returns
`pev->origin + pev->view_ofs` — the player's *eye*. That is why naive VR ports shoot out of your
face. The two existing Half-Life VR projects ([HLVR](https://github.com/maxmakesmods/halflifevr),
[Lambda1VR](https://github.com/Team-Beef-Studios/Lambda1VR)) both fix it by forking the HL SDK and
overriding that virtual function. That works, but a forked `hl.dll` only fixes the single mod it
was built for.

Instead: **`entvars_t` is engine memory — the game DLL only *reads* `view_ofs`.** So the engine
points `view_ofs` at the controller for exactly the one call that fires the weapon
(`PostThink` → `ItemPostFrame`) and restores it immediately after.

Goals:

* **Any mod works** — stock `hl.dll` untouched, no per-mod rebuild.
* **Co-op with desktop players works** — nothing added to the network protocol, so vanilla
  clients still connect.
* **Other players unaffected** — scoped to the local client, so on a listen server remote
  players are left alone.
* *Limitation:* applies when the VR player **hosts**. Joining someone else's server reverts to
  eye-origin, since that server cannot see your controller without a protocol change.

Full write-up: [`PCVR_LOG.md`](PCVR_LOG.md) → **FINDING 017**.

Other VR features: stereo OpenXR rendering, head/hand tracking, hand + weapon models,
two-handed weapon stabilisation (geometry-based grip), laser sight, grenade trajectory arc,
swing-to-hit melee, off-hand flashlight, haptics, stick locomotion with snap turn, in-headset
HUD, and a desktop mirror.

## Mod compatibility

Playing arbitrary mods means the VR layer can never assume Half-Life's own content. Three things
keep it honest:

**Weapons are classified from model metadata, not filenames.** VR has to know whether the
equipped weapon is melee (swing-to-hit), throwable (trajectory arc) or a gun (laser sight).
Matching model names against a hardcoded list (`crowbar`, `v_grenade`, ...) only ever works for
Half-Life, and silently treats a mod's custom melee weapon as a gun. Instead the engine reads the
compiled studio header — `numattachments` for a muzzle, `mstudioseqdesc_t.label` for
`throw`/`pinpull`/`fire`/`shoot` sequences. Every mod ships models carrying that same metadata,
so classification is evidence-based and needs no per-mod configuration.

**Aim comes from each weapon's own muzzle attachment.** No tuned pitch constant could ever
converge, because the error is not a constant rotation — each model's barrel sits at its own
arbitrary orientation. Reading `attachment[0] → [1]` gives the true bore line, exact and
per-weapon, for any model a mod ships.

**The fire path is self-verifying.** Substituting `view_ofs` around `PostThink` covers every
stock weapon path, projectiles included — RPG, crossbow, hornet gun, snarks and satchels all
derive their spawn point from `GetGunPosition()` inside `ItemPostFrame`. That cannot be *proven*
for a mod nobody has seen yet, though: it may override `ItemPreFrame`, or fire from a custom
think. Rather than heuristically rewriting `pfnTraceLine` — which would corrupt every monster's
line-of-sight, since `FVisible()` traces to `EyePosition()` = `origin + view_ofs` — the engine
just watches. A shot leaving the player's eye *outside* the substitution window gets logged under
`vr_debug`, naming the gap precisely.

**A fourth thing keeps it honest: the mod's own rendering still runs.** A mod signals extra
render passes with `rp.nextView` — that is what drives security-camera monitors, mirrors,
portals and render-to-texture scopes. The stereo path collects those views once per frame,
exactly as the flatscreen loop would, then replays them into each eye. Only the player's own
view takes the per-eye pose; a monitor showing a corridor keeps showing that corridor.

## What it runs today

| | |
|---|---|
| **Half-Life** | Playable |
| **Opposing Force** | Loads its own shipped `opfor.dll` — no rebuild needed |
| **Blue Shift** | Needs rebuilt DLLs; the Steam release is vgui2 and Xash cannot load it |

Most legacy mods need nothing done to them at all. They ship 32-bit game DLLs, and a 32-bit
engine loads those verbatim — which is the whole reason this fork stays 32-bit. FWGS's own
[`supported-mod-list.md`](Documentation/supported-mod-list.md) runs to ~1200 entries and is the
real compatibility list.

Where a mod's own DLL genuinely will not load, [FWGS hlsdk-portable](https://github.com/FWGS/hlsdk-portable)
carries a recreated branch for it — Blue Shift being the worked example. That is a per-mod build,
not per-mod engine work.

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
