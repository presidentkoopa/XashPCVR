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

> [!IMPORTANT]
> **Build 32-bit.** `COM_GenerateServerLibraryPath()` uses the mod's declared DLL filename
> verbatim only on 32-bit Windows; every other architecture rewrites it with an `_amd64` suffix.
> Legacy GoldSrc mods ship 32-bit game DLLs exclusively, so a 64-bit engine cannot load any of
> them without rebuilding each one from source. FWGS also documents which mods work at all —
> see [`supported-mod-list.md`](Documentation/supported-mod-list.md) and
> [`not-supported-mod-list-and-reasons-why.md`](Documentation/not-supported-mod-list-and-reasons-why.md).
