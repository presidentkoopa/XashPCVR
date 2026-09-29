# XashPCVR

A room-scale PCVR fork of Xash3D FWGS, built so that **arbitrary GoldSrc mods run in VR with
their own custom content** — not one hand-patched mod, but the twelve hundred that already
exist. Weapons fire from your controller against a completely unmodified game DLL, which is
what makes that possible: Half-Life, Opposing Force, Blue Shift and the rest load as they
shipped. On top of that sits stereo OpenXR rendering, head and hand tracking, two-handed
stabilisation, a laser sight, a grenade arc, swing-to-hit melee, an off-hand flashlight, stick
locomotion with snap turn, an in-headset HUD and a desktop mirror.

The work in progress is **real weapons**: every gun becomes a machine rather than an animation.
Parts move on measured joints, rounds are objects that live in a magazine or a chamber or your
hand, and the gun is a body with mass that your hands hold through springs — so muzzle climb,
two-handed steadiness and the weight of a rocket launcher all come out of the same numbers
instead of being scripted. A weapon is described by a *card* measured off its own model, bound
by fingerprint so the SD and HD rigs that share a filename cannot be confused; parts the
animators never made are cut from the mesh at load. The mechanism, the rounds and the fire
control are fixed-point and shared by the client and the server, so a swing, a reload and a shot
are predicted and authoritative alike — proved by running the same six thousand commands through
32-bit and 64-bit builds and comparing every byte of state.

---

*This fork is not affiliated with Valve or with the Xash3D FWGS project. Xash3D FWGS is
GPL-licensed; so is this.*
