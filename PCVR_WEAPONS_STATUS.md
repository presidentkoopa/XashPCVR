# Weapons build — where we are

Status against [`PCVR_WEAPONS_PLAN.md`](PCVR_WEAPONS_PLAN.md), part by part. Written so that
anybody — or any later session — can pick the work up without reconstructing it from commits.

Last updated 30 September 2026.

## The two documents

- [`PCVR_WEAPONS_PLAN.md`](PCVR_WEAPONS_PLAN.md) — *XashPCVR Weapons: The Full Build*, Parts A–O.
  This is the live plan. It replaced step 2 of the platform plan and Part E of it.
- [`PCVR_PLATFORM_PLAN.md`](PCVR_PLATFORM_PLAN.md) — *Platform Direction and Architecture*, Parts
  A–K and a work order of steps 0–8. **On hold** since 29 September. Steps 0 and 1 are done; step
  2 is what the weapons plan replaced. Paused, not abandoned.

Also still current: [`PCVR_FEEL_CHECKLIST.md`](PCVR_FEEL_CHECKLIST.md), which is the contract any
rewrite must not silently break. **None of it has been verified in a headset.**

## Part by part

| Part | State | Where it lives |
| --- | --- | --- |
| **B** Architecture | done | the split holds; posing via `pfnGetJointValues`, game API v4 |
| **C** Mechanism simulator | done | `hlsdk/dlls/vr_joint.*`, `vr_trigger.*` |
| **D** Rounds and feed | done | `hlsdk/dlls/vr_feed.*` — magazine, tube, belt, cylinder, single-shot, thrown |
| **E** Cards and mesh surgery | done | `hlsdk/dlls/vr_card.*`; `engine/common/mod_surgery.*`; `tools/vrcard/` |
| **F** Held weapon as an object | done, gated | `engine/client/vr/vr_hold.*`, behind `vr_hold_sim`, default 0 |
| **G** Hands on the gun | **part** | control mapping done (`VRGun_Controls`); **grip solver not built**; haptics deferred |
| **H** Sights and scopes | not started | — |
| **I** World and body | not started | — |
| **J** Half-Life's arsenal | done | 12 cards in `tools/vrcard/cards/valve/` |
| **K** Opposing Force | done | 7 cards in `tools/vrcard/cards/gearbox/` |
| **L** Malfunctions, fidelity | not started | — |
| **M** Multiplayer | not started | prediction state deliberately deferred; see below |
| **O** Testing | **part** | headless suites and cross-build determinism done; everything needing a headset not |

Against the plan's own milestones: **W1, W2 and W4 essentially done**, W3 half, W5–W9 untouched,
and **W0 is still zero** — nothing has been in a headset and the `vrcmd_t` block has never been
seen to cross a network.

## Pick up here

In the order I would take them:

1. **The grip solver (G-01).** It blocks every weapon control on every gun: a control is only
   operable while the *solved* thumb tip is inside its radius, and `controls_under` is hard zero
   until that exists. The plan says it works with the hands already in the tree — and
   `vr_hand_hevsuit.mdl` is exactly what it describes: 26 bones, five fingers with metacarpals,
   **zero sequences**, built to be posed.
2. **Part I — the world and the body.** Holsters, dropping, picking up, the pouch. Without it the
   throwables that are carded cannot actually be thrown and a dropped magazine lands nowhere. This
   is also where the plan wants a physics library (Jolt or ReactPhysics3D behind a small C wrapper);
   Part F needed none, because one body on springs is an integrator.
3. **Part H — sights and scopes.** Needs the renderer's aux-view pass. The crossbow and the M40A1
   are carded and waiting on it.
4. **Part L — malfunctions and fidelity presets.** Self-contained and headless-testable.

## Decided, so do not re-litigate

- **Fidelity default: standard. Released weapons: return to holster.** (Owner, 29 Sep.)
- **Grenades: one mechanism, both behaviours.** The pin starts nothing; your grip holds the spoon;
  a thumb control releases it deliberately, which is cooking, and throwing releases it too.
- **Prying: dropped.** There is nothing in Half-Life to pry.
- **Haptics: later.** Part G's table is written and unbuilt.
- **Prediction state does not go in `weapon_data_t`.** It has 32 spare bytes and needs ~160, and
  growing that struct corrupts memory for any mod DLL not rebuilt. It gets its own capability-gated
  channel, mirroring `clc_vrcmd`, and it belongs with the online milestone — single player is
  already correct because both sides start identical and step identically.

## Traps already paid for

Each of these cost a wrong implementation first. They are written up in full in the commits.

- **A catch being *engaged* is not the joint *resting* on it.** The slide stop engages while the
  slide is still travelling back, nowhere near the notch.
- **The hand needs a *demand* and a *clamped force*.** Physics uses the clamp; catches read the
  demand. Once both a tentative pull and a firm tug clamp to the same force, "a firm tug past the
  stop" is inexpressible.
- **Detent crossings need hysteresis.** Without it a hand resting on a mark crosses it ten-plus
  times a second.
- **Part F wants a *time*, not a stiffness.** A spring stiff enough to hold a pistol within 2 mm is
  unstable at any sub-step a 90 Hz frame can afford.
- **Recoil belongs to the cartridge.** Derive both recoil and inertia from mass and every term
  cancels: a launcher kicks exactly as hard as a pistol.
- **One bone cannot be two joints.** A part is a lerp between two bracketed poses driven by one
  value, which is why the M40A1's bolt had to be carved and its handle rewritten.
- **Joint order is not arbitrary.** The `magazine_slide` preset makes joint 0 the action and joint 1
  the magazine; numbering them otherwise silently swaps their catches.
- **Fire rates on hand-worked weapons want to be loose.** The mechanism is the limiter; a tight rpm
  adds an invisible second timer that refuses to fire when the player has done everything right.

## How to check it still works

```
hlsdk-portable\dlls\vr_test_all.bat          every headless suite
hlsdk-portable\dlls\vr_determinism_test.bat  the same stream through x86 and x64
XashFWGS\tools\vrcard\vr_hold_test.bat       Part F's claims, measured
XashFWGS\tools\vrcard\vrcardgen.py --census  re-measure any model set
hlsdk-portable\dlls\vr_card_check.bat <card> what the simulator will see
```
