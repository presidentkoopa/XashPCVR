# PCVR mod target list

Mods we want playable in VR. Cross-referenced against FWGS's own compatibility docs
([`supported-mod-list.md`](Documentation/supported-mod-list.md),
[`not-supported-mod-list-and-reasons-why.md`](Documentation/not-supported-mod-list-and-reasons-why.md))
and against the actual state of this project's builds and installs.

VR behaviour is content-agnostic by design (model-metadata weapon classification,
muzzle-attachment aim, `entvars_t` fire origin), so a mod that loads should get VR correctness
for free. That claim is proven per-mod only once someone actually plays it in a headset — see
the status column.

## Hard gates

| Gate | Effect |
|---|---|
| **vgui2** | Xash3D does not implement it. Counter-Strike 1.6, Condition Zero, Day of Defeat stay out — ruled out, see README. |
| **Custom engine** | Sven Co-op 5.0+ ships its own engine fork, not a Xash mod. |
| **Architecture** | A mod's own DLL only loads if it matches the engine's bitness. We now build both: 32-bit for the legacy catalogue (which ships 32-bit DLLs), 64-bit for standalone Xash titles that ship amd64-only. |

## Working today

| Mod | Status |
|---|---|
| Half-Life | Playable, this is the baseline the whole VR layer was built and tuned against. |
| Opposing Force | Playable — loads its own shipped `opfor.dll`, no rebuild. `run_gearbox.bat`. |
| Blue Shift | Playable — Steam `client.dll` is vgui2, so runs on the `bshift` hlsdk-portable branch instead. `run_bshift.bat`. |
| Diffusion | Engine confirmed running at 64-bit (loads amd64 game DLLs, spawns a map, OpenXR instance reaches the runtime). Stereo rendering in headset not yet verified. Needs Diffusion's own content pointed at the 64-bit build. |

## Game DLL built, content not yet installed

Built from `hlsdk-portable` branches, sitting in `E:\hlsdk-<branch>\build\`. Installing the
mod's own maps/models/sounds is what's left — the DLL is the easy part.

| Mod | Branch | Built DLL |
|---|---|---|
| They Hunger | `theyhunger` | `einar.dll` |
| Half-Life: Decay (PC port) | `decay-pc` | `decay.dll` |
| Half-Life: Echoes | `echoes` | `echoes.dll` |
| Afraid of Monsters: Director's Cut | `aomdc` | `hl.dll` |
| Poke 646 | `poke646` | `hl.dll` |
| Poke 646: Vendetta | `poke646_vendetta` | `hl.dll` |
| Half-Life: Induction | `induction_1.2` | `hl.dll` |
| Delta Particles | `delta_particles` | `delta_particles.dll` |
| Deathmatch Classic | `dmc` | `dmc.dll` (nostalgia pick, multiplayer-only) |
| Absolute Redemption | `redempt` | `redempt.dll` |
| Half-Life: Urbicide | `hl_urbicide` | `hl.dll` |

## Next up — not yet built

Azure Sheep, Counter-Life, Half-Life: Insecure, Half-Life: Top-Down, Half-Life: Visitors,
Half-Rats: Parasomnia, Half-Life: Field Intensity, and the They Hunger-family mod set (USS
Darkstar, The Gate, TWHL Tower, Halfquake / Trilogy). All have `hlsdk-portable` branches or are
in FWGS's supported list; none have been fetched/built yet.

## Ruled out

| Mod | Reason |
|---|---|
| Counter-Strike 1.6 / Condition Zero / Day of Defeat | vgui2. Dead end, not revisiting — recreated client DLLs (`cs16-client`-style) are the only route if this ever changes. |
| Sven Co-op 5.0+ | Own engine fork. |
| Half-Life: Extended | Long-term target, once it actually releases — hooks GoldSrc internals directly, real work when the time comes. |

## Team Fortress Classic — partial

`Velaron/tf15-client` is a client-only reimplementation; there's no `tfc` branch in
`hlsdk-portable`. Server side needs the real `tfc.dll`, which means owning TFC as a separate
Steam app. Wanted for nostalgia; blocked on content, not engine work.
