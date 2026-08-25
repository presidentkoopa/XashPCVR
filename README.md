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

Consequences:

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
