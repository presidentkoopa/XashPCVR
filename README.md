# XashPCVR

A room-scale PCVR fork of Xash3D FWGS, built so **arbitrary GoldSrc mods run in VR with their own
content** — not one hand-patched mod, but the twelve hundred that already exist. We ship the engine;
you bring your own Half-Life. Everything here runs against a **completely unmodified game DLL**, which
is the whole trick: Half-Life, Opposing Force, Blue Shift and the rest load exactly as they shipped,
and so does a mod nobody has ever heard of.

**You have a body.** Room-scale movement with a world reference that follows you, physical crouching
that the game reads as crouching, stick locomotion with snap turn, and teleport with an aiming arc for
anyone who wants it. Ladders are climbed **hand over hand** — you reach up, take hold, and pull
yourself. There is a seated mode that re-roots everything to the chair. Audio is listened to from your
head, not the player entity.

**You have hands, and they do things.** You press buttons and pull levers by reaching for them with a
hand rather than looking at them. The flashlight shines from the hand holding it. Thrown objects leave
**at the speed of the hand that threw them**, so an underarm lob and a full overarm throw land in
different places — which stock Half-Life, throwing everything at a constant, cannot do. Melee is
swing-to-hit. You can **dual wield**, with the off hand's muzzle and aim handed to the game DLL
through entity fields that stock Half-Life never reads, so a mod that does not understand them is
simply unaffected. Reloads are gestures: drop a magazine, swing out a cylinder, and the round in your
hand is drawn in your hand. All of it has **haptics** — fire, impact, a magazine seating, a cylinder
latching.

**And the shot comes out of the barrel.** Not out of the camera. The engine substitutes the real muzzle
of the real weapon model as the firing origin for exactly the bracket in which the mod traces its
bullet, then puts everything back — so autoaim, trace limits and the player's own view never observe
the substitution. That substitution is also three floats and a subtraction with no VR code behind it,
which means **a dedicated server can host VR-correct players without OpenXR, a headset, or any of the
VR client compiled in at all.**

The work in progress is **real weapon mechanics** — parts on measured joints, rounds as objects, and a
gun that is a body with mass your hands hold through springs. That half lives in
[XashPCVR-hlsdk](https://github.com/presidentkoopa/XashPCVR-hlsdk), along with a simulator proven
identical across nine build configurations. This repo holds the engine side of it: model fingerprints,
mesh surgery, the card toolchain, and the held-weapon physics.

| Feature | Status |
| --- | --- |
| Stereo OpenXR rendering, MSAA and supersampling, desktop mirror | working |
| Head and hand tracking, head-anchored listener, in-headset HUD and menus | working |
| Room-scale movement, physical crouch, stick locomotion, snap turn, seated mode | working |
| Teleport locomotion with an aiming arc | working |
| Hand-over-hand ladder climbing | working |
| Firing from the weapon's real muzzle, against an unmodified game DLL | working |
| Two-handed stabilisation, laser sight, grenade arc, swing-to-hit melee | working |
| Reach-to-use: buttons, levers and doors by hand | working |
| Flashlight from the hand that holds it | working |
| Throw velocity taken from the hand | working |
| Dual wielding, with the off-hand pose handed to the game DLL | working |
| Reload gestures — drop a magazine, swing a cylinder, a round drawn in your hand | working |
| Haptics on fire, impact and mechanism events | working |
| Weapon-angle calibration and per-model alignment | working |
| VR-correct players on a dedicated server with no VR code compiled in | working |
| Hand-state channel (`vrcmd_t` v4); single player runs the same wire as a network game | working |
| Weapon cards bound by model fingerprint; generator, census, mesh preview, completeness audit | working |
| Mesh surgery — carve a bone the animators never made, or rewrite one's motion | works in isolation; **cannot currently bind**, see below |
| Held weapon as a body with mass, recoil and comfort caps | built, behind `vr_hold_sim`, off by default |
| Grip solver and thumb-on-control detection | not built — blocks every weapon control |
| Sights, scopes and magnification | not started |
| World objects: dropping, placing, holsters, the pouch | not started |
| Player IK and a visible body | not started |
| Haptics for weapon mechanism events specifically (Part G's table) | designed, not built |

**Known blocking defect.** A card that declares a synthetic part cannot bind. `Mod_StudioFingerprint`
reads the cached model, and mesh surgery replaces that cache during load, so the fingerprint is taken
from the *grown* model while the card declares what the generator measured from the file on disk. The
surgery still runs — it is driven by a deliberately unchecked query — so the result is a carved mesh
with vanilla behaviour. The fix is to capture the fingerprint from the pre-surgery bytes, because a
fingerprint's job is to identify the file the artist shipped.

**Cards** live under `tools/vrcard/cards/` and install to `<gamedir>/vr/cards/`. A card binds to the
model it was measured from and declines every other, so an absent or mismatched card simply leaves that
weapon behaving exactly as it always has. `cards/hd/` and `cards/gearbox/` are the retail set; the
twelve under `cards/valve/` are measured against the Half-Life VR Mod's rigs and bind to nothing on a
stock install.

Current status, part by part, is in [`PCVR_WEAPONS_STATUS.md`](PCVR_WEAPONS_STATUS.md).
**Nothing here has been tested in a headset.**

*Not affiliated with Valve or the Xash3D FWGS project. Xash3D FWGS is GPL-licensed; so is this.*
