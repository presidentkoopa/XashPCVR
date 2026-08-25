# PCVR mod target list

Mods we want playable in VR, cross-referenced against FWGS's own compatibility docs
([`supported-mod-list.md`](Documentation/supported-mod-list.md),
[`not-supported-mod-list-and-reasons-why.md`](Documentation/not-supported-mod-list-and-reasons-why.md))
and against mod-specific handling that already exists in this engine.

Status here is about **whether the engine can run the mod at all**. It is a separate question
from whether our VR layer behaves correctly once it does — that part is content-agnostic by
design (model-metadata weapon classification, muzzle-attachment aim, `entvars_t` fire origin),
so a mod that loads should get VR for free. Tier A is where to prove that.

## Hard gates

Three things stop a mod dead regardless of how good our VR layer is:

| Gate | Effect |
|---|---|
| **Architecture** | We build 32-bit. Legacy GoldSrc mods ship 32-bit game DLLs, so this is the correct side. A 64-bit engine cannot load any of them. |
| **vgui2** | Xash3D does not implement it. Any mod requiring it will not run. |
| **Custom engine** | Some "mods" ship their own engine fork and are not Xash targets at all. |

---

## Tier A — should run as shipped

Highest value: these prove the VR layer against non-Half-Life content with no extra work.

| Mod | Notes |
|---|---|
| They Hunger / Trilogy | Best first target. Game DLL source is already on this machine (`E:\Lambda1VR\...\hlsdk-xash3d_theyhunger`), so behaviour can be cross-checked against real code. FWGS also lists a They Hunger-based mod family. |
| USS Darkstar | Listed. |
| The Gate | Listed. |
| TWHL Tower | Listed. |
| Halfquake / Trilogy | Listed. |
| Half-Life: Field Intensity | Listed. |
| Half-Life: Intense Force | Listed. |
| Half-Rats: Parasomnia | Listed. |
| Featureful-based projects | Listed; several entries. |

## Tier B — listed, but need libraries rebuilt from `hlsdk-portable`

These run, but their original DLLs are not usable as-is on all platforms; FWGS maintains
recreated source. Cost is a game-DLL build per mod, not engine work.

Afraid of Monsters + Director's Cut · Azure Sheep · Counter-Life · Absolute Redemption ·
Half-Life: Echoes (`echoes` branch) · Half-Life: Induction · Half-Life: Insecure ·
Half-Life: Top-Down · Half-Life: Urbicide · Half-Life: Visitors · Half-Life: Decay (PC port) ·
Half-Life: Blue Shift (`bshift` branch — the Steam release itself is vgui2-blocked)

> This is also the escape hatch for the 32/64-bit split: `hlsdk-portable` can be compiled for
> either, so anything in this tier could follow us to a 64-bit engine later.

## Tier C — supported, but the engine already special-cases them

Worth watching during VR testing, since engine behaviour is not stock here.

| Mod | Existing handling |
|---|---|
| Team Fortress Classic | Class config hacks — `engine/client/parse/cl_parse.c:2161`, `:2164`, `engine/common/cmd.c:1271` |
| Half-Life: Invasion | Godmode/notarget dev workaround — `engine/server/sv_client.c:1367` |
| Ricochet | Cvar filter exception — `engine/common/cvar.c:39` |
| Cry of Fear | Antisave protection disabled — `engine/client/parse/cl_parse.c:2425`. Note: primarily a standalone-engine title; treat as low priority. |

Some mods additionally need `-bugcomp` flags (`engine/common/common.h:268-278`,
[`bug-compatibility.md`](Documentation/bug-compatibility.md)).

## Tier D — blocked

| Mod | Reason | Escape route |
|---|---|---|
| **Sven Co-op 5.0+** | Uses a custom GoldSrc engine | None. Painful, since this is *the* co-op mod. |
| **Day of Defeat** | Requires vgui2 | FWGS `vinterface` branch is unfinished |
| **Half-Life: Extended** | Hooks GoldSrc engine internals + version check | Older build may work |
| **Diffusion** | Ships 64-bit only (`bin/server_amd64.dll`) — confirmed on disk at `D:\SteamLibrary\steamapps\common\Diffusion` | Needs our 64-bit engine build |

## Tier E — unlisted, needs investigation

Not found in either FWGS document. Status genuinely unknown — do not assume either way.

Gunman Chronicles (retail standalone, own game DLL) · Heart of Evil · Nightmare House ·
Half-Life: Rally · Half-Life: Reprocessing · Half-Life: Update

## Multiplayer / co-op

Secondary priority — walk away from it if it fights back.

| Mod | Status |
|---|---|
| Deathmatch Classic | Listed |
| Ricochet | Listed, see Tier C |
| Team Fortress Classic | Listed, see Tier C |
| Natural Selection | Listed |
| Adrenaline Gamer | Listed |
| Sven Co-op | **Blocked**, see Tier D |
| Day of Defeat | **Blocked**, see Tier D |

The VR fire-origin work is already co-op-safe: it is scoped to the local client
(`NET_IsLocalAddress`) and adds nothing to the network protocol, so desktop players can join a
VR-hosted server normally. The limitation is the reverse direction — a VR player joining
someone else's server reverts to eye-origin.

## Standalone Xash games ("custom exe")

Games shipping their own Xash3D FWGS build rather than running as a mod. Diffusion is the
worked example: `gameinfo.txt` instead of `liblist.gam`, its own `bin/` with
`server_amd64.dll` / `client_amd64.dll` / `menu_amd64.dll`.

Hooking these means matching their architecture, and they are trending 64-bit. This is the
main argument for eventually producing a 64-bit VR engine alongside the 32-bit one.
