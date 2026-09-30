# XashPCVR

A room-scale PCVR fork of Xash3D FWGS, built so **arbitrary GoldSrc mods run in VR with their own
content** — not one hand-patched mod, but the twelve hundred that already exist. Weapons fire from
your controller against a completely unmodified game DLL, which is what makes that possible:
Half-Life, Opposing Force, Blue Shift and the rest load as they shipped.

The work in progress is **real weapons**. Parts move on joints measured off each model, rounds are
objects that live in a magazine or a chamber or your hand, and the gun is a body with mass your
hands hold through springs — so muzzle climb, two-handed steadiness and the weight of a launcher
come out of the same numbers instead of being scripted. A weapon is described by a *card* measured
from its own model and bound by fingerprint, so the SD and HD rigs that share a filename cannot be
confused; parts the animators never made are cut out of the mesh at load. The game-side half lives
in [XashPCVR-hlsdk](https://github.com/presidentkoopa/XashPCVR-hlsdk).

| Feature | Status |
| --- | --- |
| Stereo OpenXR rendering, head and hand tracking | working |
| Weapons fired from the controller by unmodified game DLLs | working |
| Two-handed stabilisation, laser sight, grenade arc, swing-to-hit melee | working |
| Flashlight, stick locomotion with snap turn, in-headset HUD, desktop mirror | working |
| Hand-state channel (`vrcmd_t` v4: parts, trigger axis, buttons, muzzle) | built; never yet seen crossing a network |
| Weapon cards, bound by model fingerprint | working |
| Mesh surgery — carve a new bone, or rewrite an existing one's motion | working, verified against the originals |
| Card generator, census and mesh preview (`tools/vrcard`) | working |
| Held weapon as a body with mass, recoil and comfort caps | built, behind `vr_hold_sim`, off by default |
| Posing from the simulator's predicted joint values | working |
| Grip solver and thumb-on-control detection | not built — blocks every weapon control |
| Sights, scopes and magnification | not started |
| World objects: dropping, placing, holsters, the pouch | not started |
| Haptics | deferred |

*Not affiliated with Valve or the Xash3D FWGS project. Xash3D FWGS is GPL-licensed; so is this.*
