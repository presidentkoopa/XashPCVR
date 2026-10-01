# XashPCVR

A room-scale PCVR fork of Xash3D FWGS, built so **arbitrary GoldSrc mods run in VR with their own
content** — not one hand-patched mod, but the twelve hundred that already exist. We ship the engine;
you bring your own Half-Life. Weapons fire from your controller against a completely unmodified game
DLL, which is what makes that possible: Half-Life, Opposing Force, Blue Shift and the rest load as
they shipped.

What is being built is a **real weapon simulator**, and that is the unusual part. Joints with springs,
catches, detents, gates and crossing events. A feed path where rounds are objects that move between
magazine, tube, belt, cylinder, chamber and your hand — and only ever because a part crossed a point
or a hand put them there. A trigger with travel and a sear that breaks. Six weapon archetypes as
presets. All of it fixed-point, so the client and the server reach the same answer bit for bit.

That last property is rarer than it sounds and worth more than it sounds. **Nine build configurations
— including x87, where floating-point intermediates live in 80-bit registers rather than SSE's 32 —
agree on twelve thousand commands across all six weapon kinds.** And the test is known to bite: three
float leaks injected into a copy of the simulator, two caught, and the third one provably incapable of
diverging. Most VR weapon systems cannot make that claim, because most of them never needed to. This
one does, because `hl.dll` and `client.dll` are separate binaries and netplay is two machines running
builds nobody coordinated.

Around it: mesh surgery that can carve a bone the animators never made and write its motion, rotation
included. A card format bound by model fingerprint, so the SD and HD rigs that share a filename cannot
be confused. A generator that measures a model and drafts its card. A melee sweep that reads the
weapon's head between commands. Held-weapon physics with mass and recoil. Twenty-five cards bound to
retail content. One hundred and seven headless test cases across seven suites.

| Feature | Status |
| --- | --- |
| Stereo OpenXR rendering, head and hand tracking | working |
| Weapons fired from the controller by unmodified game DLLs | working |
| Two-handed stabilisation, laser sight, grenade arc, swing-to-hit melee | working |
| Flashlight, stick locomotion with snap turn, in-headset HUD, desktop mirror | working |
| Fixed-point weapon simulator, identical on client and server | working, 107 headless cases |
| Determinism across nine build configurations, x87 included | verified, 12,000 commands |
| Hand-state channel (`vrcmd_t` v4: parts, trigger axis, buttons, muzzle) | working; single player now runs the same wire as a network game |
| Client prediction of the simulator | repaired — had never executed; not yet run in a headset |
| Weapon cards, bound by model fingerprint | working |
| 18 HD cards (15 weapons × 3 games) + 7 for Opposing Force's own arsenal | 6 verified clean, 12 with known fixes outstanding |
| Card generator, census, mesh preview and completeness audit (`tools/vrcard`) | working |
| Mesh surgery — carve a new bone, or rewrite an existing one's motion | works in isolation; **cannot currently bind**, see below |
| Held weapon as a body with mass, recoil and comfort caps | built, behind `vr_hold_sim`, off by default |
| Grip solver and thumb-on-control detection | not built — blocks every weapon control |
| Sights, scopes and magnification | not started |
| World objects: dropping, placing, holsters, the pouch | not started |
| Malfunctions and fidelity presets | not started |
| Haptics | deferred |

**Known blocking defect.** A card that declares a synthetic part cannot bind. `Mod_StudioFingerprint`
reads the cached model, and mesh surgery replaces that cache during load, so the fingerprint is taken
from the *grown* model while the card declares what the generator measured from the file on disk. The
surgery still runs — it is driven by a deliberately unchecked query — so the result is a carved mesh
with vanilla behaviour. The fix is to capture the fingerprint from the pre-surgery bytes, because a
fingerprint's job is to identify the file the artist shipped.

**Cards** live under `tools/vrcard/cards/`, sorted by game directory, and install to
`<gamedir>/vr/cards/`. A card binds to the model it was measured from and declines every other, so an
absent or mismatched card simply leaves that weapon behaving exactly as it always has. The twelve
cards under `cards/valve/` are measured against the Half-Life VR Mod's rigs rather than retail
Half-Life and bind to nothing on a stock install; `cards/hd/` and `cards/gearbox/` are the retail set.

Nothing here has been tested in a headset.

*Not affiliated with Valve or the Xash3D FWGS project. Xash3D FWGS is GPL-licensed; so is this.*
