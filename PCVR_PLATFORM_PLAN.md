# XashPCVR — Platform Direction and Architecture

Sep 28, 2026 · @Simon

XashPCVR is a 64-bit VR platform for Xash-based games, with Half-Life, Opposing Force and Blue Shift as its flagship. The plan: make the three campaigns playable end to end first, move weapon state out of the engine into game code we compile so reloading predicts and networks correctly, then spend the engine's headroom on body, world interaction and a modern renderer. This brief is for whoever writes the code; it builds on the Project Review, `PCVR_CARD_PLAN.md` and a read of the current source (`vr_openxr.c`, `gl_studio.c`, `hlsdk-portable`).

**How this plan is organised.** Parts A and B apply to everything. Parts C to I are the work, one part per step of the work order, in the order it gets done. Parts J and K support all of it.

| Part | Covers | Work order step |
| --- | --- | --- |
| A | Ground rules: decisions, what "platform" means, netplay rules | all |
| B | Work order | all |
| C | Fix now, and docs to correct | 0 |
| D | The 64-bit switch | 1 |
| E | Weapons: reload architecture, hand-event channel, cards, census, weapon mechanics | 2 |
| F | Finishing Half-Life, Opposing Force and Blue Shift | 3 |
| G | Body and arms | 4 |
| H | VR mechanics: throwing, climbing, touching the world | 5 |
| I | The renderer | 6 (R0 any time) |
| J | Testing with one PC and one tester | starts in step 2 |
| K | Open questions for the owner | — |

Netplay itself (step 7) has no part of its own: its rules are in Part A and bind every other part, and its build-out follows the campaign.

# Part A — Ground rules

## Decisions already made

These come from the owner and are not open for debate. Plan around them.

| Decision | What it means for the code |
| --- | --- |
| **64-bit is the primary build** | The 32-bit build keeps compiling as a fallback but stops being the one we test. Closed 32-bit mod DLLs are lost; open mods run on DLLs we build from hlsdk-portable. |
| **Single-player Half-Life first** | Everything before the netplay step is judged by one question: does it make a full campaign playthrough in the headset better? |
| **Netplay is designed in now, built later** | Nothing built from today may need a rewrite to go online. The rules are in *Netplay rules* below. |
| **Any engine work is authorized** | Protocol, renderer, ABI between engine and our game DLLs: all fair game. The owner wants visuals expanded later. |
| **Netplay must not break between our clients** | Two players on this engine must always be able to play together, VR or desktop, co-op or deathmatch. Compatibility with stock GoldSrc or vanilla Xash is nice to keep, not required. |
| **Headset brand does not matter** | Target OpenXR generically. 64-bit also makes SteamVR work, which 32-bit could not. |
| **One PC, one tester** | There is no second machine. Every netplay test runs on one PC, and anything testable without a headset should be. |
| **Diffusion comes later** | Keep the 64-bit build able to load it; do no Diffusion-specific work yet. |

**What "platform" means for the code.** Half-Life and its expansions are where every feature is proven first. A platform then needs three more things, and today's work should not close them off:

1. **VR that reaches any Xash game DLL.** The engine-side path — muzzle aim, hands, parts, the fallback reload — must keep working on game code we have never seen.
2. **A published VR interface for authors.** The `vrcmd_t` channel, `GetVRWeaponAPI` and the card format become a versioned, documented SDK, so a mod or standalone game can adopt full hand-driven weapons without us.
3. **Games that draw the world themselves.** Standalone Xash games built on client-side renderers (PrimeXT/XashXT style; Diffusion is one) bypass our stereo pass. They need an engine contract that asks the client renderer to draw once per eye with our view and projection. That is post-flagship work, but the contract should be designed before the VR interface is published, not after.

## Netplay rules for every change from now on

Netplay is built in step 7, but anything written before then must already obey these, or step 7 becomes a rewrite.

1. **Our engine talks to our engine.** Protocol changes are allowed. Put each one behind a capability bit in the existing `ext` handshake, and refuse a mismatched build at connect with a clear message naming both versions. Never let two different builds connect and quietly disagree.
2. **Per player, never global.** Anything that changes game rules for a VR player keys off that player's userinfo, the way `vr_handload` does, so VR and desktop players share a server and each gets their own rules.
3. **The server decides; the client predicts.** Ammunition, firing, damage and use are decided by server code. The client may predict them with the same code, never decide them alone.
4. **Shared code is deterministic.** Same inputs as listed under the event channel: the command, the `vrcmd_t`, the weapon state and the shared seed. Nothing else.
5. **Stop special-casing the host.** Today the server's VR substitutions are limited to the local player with `NET_IsLocalAddress` (`sv_pmove.c:977` and `:1074`, per `PCVR_LOG.md` Finding 018), so a VR player hosting and a VR player joining get different treatment. Replace that with the per-player capability: every VR player is treated the same whether local or remote.
6. **A dedicated server needs no VR hardware.** It must still run everything a VR player needs from the server: reading pose and `vrcmd_t`, and running `vr_weapon`. Only client-side OpenXR code sits behind `!XASH_DEDICATED`.
7. **What other players see comes from the server.** Part positions for another player's weapon go in that player's entity state (the directions document suggests the networked `controller[]` bytes; with our own protocol a dedicated field is cleaner). Hands and body for other players belong to step 7 and need the third-person weapon model to carry parts as well.
8. **Co-op engine limits to lift in step 7** (static analysis in `PCVR_LOG.md` Finding 018, not yet run):
   - saving refuses whenever `maxclients != 1` (`sv_save.c:2173–2175`);
   - level transitions drop entity carry-over when `maxclients > 1` (`sv_game.c:747–748`);
   - `coop 1` should select single-player rules with several clients, but that has never been run;
   - single-player assumptions inside game code (scripted sequences that look for "the player", single spawn points) get patched in our DLLs.
9. **Deathmatch fairness is a server setting, not a hardcoded answer.** Loading by hand is slower than pressing R. Give the server a cvar that allows or disallows the classic reload for VR players, and leave the default to the owner.
10. **Every feature states what the server sees.** One line in its commit or doc: local-only, predicted, or server-decided. That habit is cheaper than auditing later.

# Part B — Work order

## Work order

Do these in order. Steps 0–6 serve the single-player campaign; step 7 turns on netplay, which the earlier steps must already be ready for.

| # | Step | Delivers | Done when |
| --- | --- | --- | --- |
| 0 | **Fix-now bugs** | The small confirmed bugs below, fixed on the current tree before anything moves | Desktop player sees the pump and slide animate; no predicted reload on an empty gun; frame time drops with a weapon out |
| 1 | **64-bit switch** | 64-bit default; Half-Life, Opposing Force and Blue Shift game DLLs built 64-bit from hlsdk-portable with our changes | All three campaigns load their first map in the headset on the 64-bit build |
| 2 | **Weapon rework** | Shared weapon mechanics in our game code, the hand-event channel, cards generated from the model | MP5, crossbow and RPG work by hand; shotgun, pistol and revolver feel no worse than today (checklist below) |
| 3 | **Campaign playthrough** | Owner plays Half-Life start to finish; coder fixes what breaks: trains, tank turrets, long jump, Xen, weapon selection | Credits roll in the headset |
| 4 | **HEV arms** | Forearms drawn from the solved torso; full arm IK later | Looking down reads as arms, and they never fight the aim |
| 5 | **World by hand** | Valve wheels and cranks turned by hand; buttons pressed by touch (see `PCVR_VR_DIRECTIONS.md`) | Reactor and valve puzzles in Half-Life played with hands only |
| 6 | **Visuals** | Renderer stages R1–R3: modern pipeline, HDR, lighting (see Expanding the renderer; R0 headroom can start any time) | Owner signs off |
| 7 | **Netplay** | One-PC test harness, other players see your hands and weapon, co-op, then deathmatch | VR and desktop player finish a map together; a deathmatch round runs clean at 100 ms simulated lag |
| 8 | **Later** | Melee from the weapon's real shape, the mod long tail, Diffusion | — |

The test harness in step 7 is also worth starting early in a small form, because a headset-free way to replay a session saves the owner hours from step 2 onward. See *Testing*.

# Part C — Fix now (step 0)

## Fix now (step 0)

Each of these was confirmed by reading the source on disk as of 28 September, not inferred from symptoms. Line numbers are for that tree. Items 1, 2 and 5 are small and should land before any restructuring, so the rework starts from a correct baseline.

1. **Weapon parts are frozen for desktop players.** Confirms the review's top unverified finding.
   - Where: `refState.vrParts[i].value` starts at 0, and `R_StudioApplyHandAction` (`gl_studio.c:3590`) treats anything ≥ 0 as "drive this bone". Only `VR_UpdateParts` ever writes -1 ("leave alone"), and it runs only from `VR_BeginFrame` once an OpenXR session is running (`vr_openxr.c:10931`).
   - Effect: with no headset, every bone named in `r_vr_action_bone` is pinned at rest — shotgun pump, pistol slide, MP5 clip, crossbow, revolver, RPG rocket, grenade pin.
   - Fix: make driving explicit. Add a `driven` flag to `vr_part_t` that defaults to false, and have the renderer leave the bone alone unless it is set. Do not rely on a sentinel value inside `value`; the card plan already names that sentinel as the root of the snapping and freezing bugs. Bump `REF_API_VERSION` — the VR fields were added without bumping it.
2. **Client prediction starts a reload the server never does.**
   - Where: the server's automatic reload on an empty gun is gated on `UTIL_PlayerHandLoads` (`hlsdk-portable/dlls/weapons.cpp:802`). The client's copy of the same logic is not (`cl_dll/hl/hl_weapons.cpp:375`).
   - Effect: for a hand-loading player, an empty gun plays a predicted reload that the server then contradicts.
   - Fix: add the same `!UTIL_PlayerHandLoads( m_pPlayer )` gate at line 375. Then diff every other path the two copies share for the same kind of gap.
3. **Magazine drop, cylinder dump and "action worked" are never predicted.**
   - Where: impulses 210–212 are handled in `CBasePlayer::ImpulseCommands` (`dlls/player.cpp:3539–3565`), which only the server runs. The client-side `DropMagazine` and `DumpCylinder` in `hl_weapons.cpp` have no caller.
   - Effect: the ammo count waits a network round trip, then jumps. This is the jitter the review predicted.
   - Interim fix: in `HUD_WeaponsPostThink`, dispatch `cmd->impulse` 210–212 to the same weapon methods before `ItemPostFrame`, so both sides run the same code on the same command. The real fix is the event channel in step 2.
4. **Releasing a slide off its catch never tells the game DLL.**
   - Where: `VR_UpdateParts` sets `vr.act_worked = true` (`vr_openxr.c:5976`); `VR_UpdateAction` runs next in the same frame and clears it unconditionally (`vr_openxr.c:7948`).
   - Effect: impulse 210 is never sent for that path. Harmless on Half-Life's pistol, which ignores it, but wrong.
   - Fix: clear `act_worked` where it is consumed (`VR_GetActionImpulse`), not at the top of `VR_UpdateAction`.
5. **The gun is re-measured on almost every frame.**
   - Where: `R_StudioApplyHandAction` is called from `R_StudioSetupRenderer` (`gl_studio.c:3157`) for every studio model drawn, not just the viewmodel. `R_StudioFindParts` caches only the last header it saw (`gl_studio.c:1226`), so any other model drawn in between throws the cache away. Each draw also resets `vrPartCount` to 0 (`gl_studio.c:3541`).
   - Effect: the viewmodel's parts are re-derived per eye per frame — every sequence sampled nine times, every vertex scanned twice. And part publication depends on the viewmodel happening to be the last studio model drawn.
   - Fix: run it only for `tr.viewent` (the hide-bone code right below already checks this at line 3165), and cache per model rather than one slot.
6. **Switching weapons leaves hand state behind.**
   - Where: the weapon-change reset in `VR_UpdateAction` (`vr_openxr.c:7971–7992`) clears the cylinder, magazine and action flags but not `part_held`, `part_value[]`, `part_grab_*`, `act_rearm`, `act_settle` or `rl_holding`.
   - Effect: a part held across a switch carries its index and value onto the new weapon.
   - Fix: one reset function that clears all per-weapon state, called from a single place. Note that this reset fires on any weapon ID change, so the review's "lower-numbered slot" theory does not hold for this function; the gun-won't-fire-after-interrupted-reload report needs reproducing.
7. **Controllers never came alive in the 64-bit Diffusion run.** In `XashVR64/vr_diag.log` (7 September), both hands read `INVALID` and the stick `UNBOUND` for the whole 43 seconds, while head tracking and frame submission worked. It may just mean the controllers weren't in hand, but it is the first thing to check on the 64-bit build in step 1.

## Docs that disagree and need correcting

The repository's documents are the project's memory, and several now contradict each other or the owner's decisions. Fix them in step 0 so nobody plans from a stale one.

| Document | Says | Now | Change |
| --- | --- | --- | --- |
| `README.md`, `PCVR_64BIT_PLAN.md`, `PCVR_MOD_TARGETS.md` | 32-bit is the default; migrating to 64-bit is out of scope | Owner chose 64-bit as primary | Rewrite the architecture sections; 32-bit becomes the fallback |
| `README.md` | Hosting and joining "both work"; VR-to-VR crossplay "Built" | The Project Review says online play has never been tested on two machines | Say "written, never run" until the one-PC test passes |
| `README.md` | "Nothing is added to the network protocol. Vanilla clients still connect." | Protocol changes between our builds are now allowed (Netplay rule 1) | State the new rule |
| `PCVR_CARD_PLAN.md` §5 | The mod owns ammunition, so hand loading cannot desync | Superseded by shared `vr_weapon` code on 64-bit | Replace §5 with the architecture in this brief |
| `PCVR_LOG.md` Finding 019 | Weapon selection is cycle-only; there is no teleport | The review lists headset weapon selection as working, and `vr_teleport` exists in the code | Mark Finding 019 superseded |
| `PCVR_LOG.md` feature priority | Networking is "explicitly low priority" | Owner: netplay is critical and designed in now | Replace with this brief's work order |
| Project Review, outstanding issues | Weapon-change state may not reset on a lower-numbered slot | The reset fires on any weapon change, but misses several fields (Fix now, item 6) | Update after reproducing the reported fault |
| Project Review, outstanding issues | Desktop weapon parts may be frozen (unconfirmed) | Confirmed (Fix now, item 1) | Mark confirmed, then fixed |

# Part D — The 64-bit switch (step 1)

## The 64-bit switch (step 1)

The engine side is mostly done: `PCVR_64BIT_PLAN.md` records a working 64-bit OpenXR loader, the x64 runtime resolving, and a 64-bit engine loading amd64 game DLLs. What remains is making it the default and giving it games to run.

1. **Make 64-bit the default build and launcher.** Keep the 32-bit configuration compiling in its own output tree (`build` vs `build64`) and build both on every change, so the fallback does not rot.
2. **Build the game DLLs 64-bit from hlsdk-portable, with our changes.** Half-Life first, then Opposing Force (today it runs Gearbox's shipped 32-bit `opfor.dll`, which a 64-bit engine cannot load, so it needs the `opfor` branch) and Blue Shift (already on the `bshift` branch). Confirm the file names the 64-bit engine asks for: `COM_GenerateServerLibraryPath` (`engine/common/lib_common.c`) adds an `_amd64` suffix off 32-bit Windows.
3. **Make our hlsdk changes portable across branches.** Today they exist as one patch against the Half-Life branch (`PCVR_HLSDK_CHANGES.patch`), and the `hlsdk-portable` tree on disk has already moved past it. The `vr_weapon` code from step 2 should be a few self-contained files plus a small number of hooks, so the eleven other `hlsdk-*` branches on disk can take it with little per-branch work.
4. **Settle the deployment layout.** `XashVR64` currently ships both `ref_gl.dll` and `ref_gl_amd64.dll` (and the same for menu and filesystem). Find out which names the 64-bit engine actually loads, keep only those, and write it down.
5. **Separate config and saves** between the 32- and 64-bit installs, as the 64-bit plan's risk list asks, so the two never read each other's files.
6. **Check controller input first.** The only 64-bit headset log has both hands invalid for the whole session (Fix now, item 7).
7. **Compile the VR layer with pointer-truncation warnings on** and treat them as errors. The plan says the VR code is architecture-clean; this proves it.
8. **Regression-run the 32-bit build** once at the end: it should still start Half-Life in the headset.

Done when Half-Life, Opposing Force and Blue Shift each load their first map in the headset on the 64-bit build, with hands tracked and a weapon firing from the muzzle.

# Part E — Weapons (step 2)

The reload plan reviewed, the architecture replacing it, the channel that carries hands to game code, cards, the census that measured every viewmodel, and the weapon mechanics that follow.

**Superseded as the plan of record (29 September).** The owner has made weapons the top priority and committed to the full build: simulated mechanisms, rounds as objects, the weapon as a physical object, sights and scopes, and the whole Half-Life and Opposing Force arsenal. The specification is Untitled. It replaces this Part's verb-based plan and takes precedence over the campaign playthrough (Part F); the step-0 fixes and the 64-bit switch still come first. The sections below remain as background: the census, the hand-event channel and the netplay rules all carry over.

## Review of the proposed reload plan

The direction in `PCVR_CARD_PLAN.md` is right, but it fixes the smaller half of the problem. Cards stop the engine guessing what a weapon *is*. They do nothing about who owns the weapon's *state* or how that state reaches the server, and that is where the prediction and netplay faults live.

**Keep:**

- Hand drives a part the model already has; no gesture recognition. This is the idea that made the shotgun work.
- Declare instead of guess, with measurement as the default and the card as the override.
- A validator that refuses a bad card by name instead of failing silently. The crossbow magazine that vanished for a week is exactly what it prevents.
- Phase 0: tag the commit and write the feel checklist *before* any code moves.
- One ownership state per part, replacing the -1 sentinel.
- The v1 scope cut: five behaviours, no spinning barrels or fill gauges.

**Change:**

1. **Its ammunition premise no longer holds.** The plan says the mod owns the ammunition, so pulling a magazine cannot empty the gun, and that this is what keeps it netplay-safe. On 64-bit, nearly every mod that runs is one whose game DLL we compile. So weapon state should live in our game code, shared between client prediction and server, and be driven by hand events the engine sends. That is safe for netplay because both copies run the same code on the same input — not because the engine stays out of it.
2. **Impulses are the wrong transport.** One impulse per command means two events in one frame lose one, the numbers can collide with a mod's own impulses, and client prediction never sees them. Replace them with the channel described below.
3. **The engine should stop inferring state.** Today it reads the clip count from the `CurWeapon` HUD message, which arrives a round trip after the shot, then maintains its own flags from it: `act_needs`, `act_open`, `act_back`, `act_armed`, `act_rearm`, `act_settle`, `part_off_catch`, `cyl_open`, `mag_out`. Most of the fixes recorded in the comments are patches to that inference. With a weapon DLL that knows its own state, the engine only reports hands.
4. **Measurement needs one more rule.** `R_StudioDerivePart` measures travel relative to the part's parent bone. If the parent moves during the chosen sequence — a magazine parented to the reloading hand, for example — the result is meaningless. Measure relative to the gun's root bone, and have the validator warn when the parent moves.
5. **"A guessed card for uncarded mods" should be the main path, not the fallback.** The owner's own directions document says authored data is a trap and derived data is what reaches every mod. Generate every card from the model; hand-written cards only correct the generator. Details under *Weapon cards, revised*.
6. **Fire blocking belongs in weapon code.** `VR_ActionBlocked` suppresses `IN_ATTACK` client-side. For DLLs we build, the weapon should refuse to fire because its chamber is empty, which the server and prediction then agree on by construction. Keep the engine-side block only for DLLs that don't speak the new interface.

**Rough confidence:** high that this is the right architecture; the plan's own risk still applies — feel is tuned by hand and easy to lose in a rewrite, so the checklist comes first.

## Target reload architecture

&#91;embedded content: reload architecture · engine, shared weapon code, network\]

The engine only ever says what the hands did; one copy of the weapon rules, compiled into both the client and server DLLs, decides what that means for the gun.

**Layer 1 — engine.** Keeps what already works: tracking, reach, grabbing a part, driving its bone from the hand, the renderer publishing part positions. Stops inferring weapon state from `CurWeapon`. Its output each frame is hand state, not verdicts.

**Layer 2 — shared weapon mechanics (`vr_weapon`).** New code in hlsdk-portable, compiled into `hl.dll` / `client.dll` like the rest of the shared weapon code, and therefore run by both prediction and the server on the same command. It holds per-weapon state and turns hand state into mechanical events:

| Mechanism | State | Events it derives |
| --- | --- | --- |
| Detachable magazine | seated / out; rounds in it | mag out, mag seated |
| Chamber | empty / loaded | chambered, spent |
| Action (slide, bolt, pump) | closed / open / locked back; stroke progress | stroke complete, catch released |
| Cylinder | closed / open; per-chamber loaded/spent | opened, dumped, closed |
| Tube | rounds in tube | round inserted |

The weapon fires only if the chamber is loaded and the action closed. That replaces `VR_ActionBlocked` for these DLLs. Desktop players never enter this path: it applies only when the player's `vr_handload` userinfo is set, so their weapons behave exactly as stock.

**Layer 3 — network.** Nothing extra is needed for the local player: the hand state rides the command, prediction and server run the same code, and the existing weapon-data correction handles disagreement. For other players to see your magazine and slide, the server sends part positions in the player's entity state (see *Netplay rules*).

**Fallback.** A game DLL that does not export the interface gets today's engine-only behaviour: parts move visually, `IN_RELOAD` is pulsed per insert, and the engine blocks the trigger. That covers Diffusion and any closed mod, and it must keep working.

## The hand-event channel

Send hand **state** with every command, and let the shared weapon code derive events from it. Do not send events.

**Why state, not events.** Commands are already sent redundantly (backup commands) and replayed by prediction. A state block replayed twice gives the same answer twice; an event replayed twice fires twice, and an event in a lost packet never fires. Every event-shaped bug in the current code — the one-frame `act_worked`, the edge-only magazine drop, the one-frame `IN_RELOAD` pulse per shell — disappears when the weapon code compares this command's hand state with the last one it saw.

**What the block carries** (call it `vrcmd_t`; sizes are a starting point):

| Field | Size | Meaning |
| --- | --- | --- |
| part value × up to 8 | 8 bits each | 0–255 along that part's travel, as the hand has it |
| part held mask | 8 bits | which parts a hand is on this command |
| off-hand grip | 1 bit | closed or open |
| round in hand | 2–3 bits | nothing / a round / a magazine / a speedloader |
| at loading port | 1 bit | the carried item is at the weapon's port |
| muzzle pose | existing | fold the current `impact_position` muzzle carrier into this block |

Delta-encode it against the previous command, so a still hand costs a few bits.

**Where it rides.** Do not change `usercmd_t`: its layout is part of the ABI every game DLL is compiled against (`CL_CreateMove`, `CmdStart`). Keep a parallel `vrcmd_t` per command number in the engine, send it alongside each `clc_move` when the `NET_EXT_VR` capability bit was negotiated, and store it on the server next to the command it belongs to.

**How the game DLL reads it.** Add an optional export to our game DLLs, for example `GetVRWeaponAPI( int version, const vr_engine_funcs_t *engine, vr_game_funcs_t *game )`. The engine looks it up at load time, as it already does for other optional exports; its absence means fallback mode. Through it the DLL gets one call — "the `vrcmd_t` for the command being run for this player" — which works identically on the server (the command in `SV_RunCmd`) and on the client (the command being replayed by prediction).

**Weapon state must survive prediction rollback.** Prediction restores weapon state from the last server snapshot, then replays commands. So the `vr_weapon` state (magazine seated, rounds in magazine, chamber, action position, cylinder chambers, last-seen hand state) must travel in the weapon data the server sends. Either pack it into `weapon_data_t`'s spare `iuser`/`fuser` fields — after auditing which ones each mod already uses — or, since both ends are ours, add a VR weapon state block to the weapon-data delta under the same capability bit. The second is cleaner.

**Determinism rules for shared code.** It may depend only on the command, the `vrcmd_t`, the weapon's state and the shared random seed. No `host.realtime`, no local cvars (per-player settings go through userinfo, as `vr_handload` already does), no engine-side flags.

**What stays in the engine.** Everything about feel that does not change the game: haptics, the stroke sound that starts with the motion, the magazine shrinking away, the recoil kick on a self-loader's slide. These read the predicted weapon state and never write it.

**Retire** impulses 210–212, the `IN_RELOAD` pulse per insert (for DLLs with the interface), `VR_ActionBlocked` (same), and the engine's clip tracking from `CurWeapon`.

## Weapon cards, revised

A card binds a model's bones to parts and names one firearm type; the rules for that type live once, in `vr_weapon`. Every card starts life generated from the model, and a hand-written card only corrects what the generator got wrong.

**Firearm types** (v1 — covers Half-Life, Opposing Force and Blue Shift):

| Type | Parts | Loading | Examples |
| --- | --- | --- | --- |
| `magazine_slide` | magazine, slide (locks back on empty) | pull magazine, seat new one, release or rack slide | 9mm pistol |
| `magazine_bolt` | magazine, bolt or charging handle (optional) | same; with no bolt bone, chamber on seating | MP5 (its model has no bolt) |
| `tube_pump` | fore-end | one round per insert, pump to chamber | shotgun |
| `revolver` | cylinder, speedloader | open, dump muzzle-up, load, flick shut | .357 |
| `single_shot` | breech or the round itself | one round, no magazine | crossbow, RPG |
| `none` | — | no hand loading; parts may still move | Gauss, Egon, Hornet gun, grenades |

Per part, a card can state: `bone`, `role` (magazine / action / cylinder / loader / round), `return = spring | hand | stay` (the plan's load-bearing key), `lock_on_empty`, and overrides for any measured number (travel, axis, extent, grab point).

**Format.** One file per game directory, `<gamedir>/vr/weapons.txt`, with a built-in default for Half-Life. It replaces the `r_vr_action_bone` cvar and its 255-character entry limit. The bindings below are the ones in today's cvar; the syntax is a sketch, not a spec:

```
weapon v_shotgun
    type   tube_pump
    part   action  bone "Charger"       return hand

weapon v_9mmhandgun
    type   magazine_slide
    part   action  bone "Hands mesh 2"  return spring  lock_on_empty

weapon v_9mmar
    type   magazine_bolt
    part   magazine bone "clip"
    chamber_on_seat

weapon v_357
    type   revolver
    part   cylinder bone "revolver"
    part   loader   bone "speed_loader"
```

**The generator** runs at weapon load when no card exists, logs its result as a guess, and writes the card it made so it can be reviewed and promoted:

1. **Find the gun body.** The bone carrying most of the non-arm geometry. Arm and hand geometry is identified by texture (`r_vr_arm_textures` already does this) and by the three-joint finger chains every Valve viewmodel carries.
2. **Find candidate parts.** Bones owning a real share of non-arm vertices, other than the body.
3. **Measure each candidate relative to the gun body** across the fire, reload and draw sequences, not relative to its parent. Keep it only if its motion is essentially one axis — one dominant direction of translation, or one rotation axis — and returns to rest.
4. **Classify by geometry**, using the muzzle attachment as "forward":
   - translates along the barrel, above the grip → slide or bolt
   - translates along the barrel, ahead of the grip → fore-end (pump)
   - translates across the barrel, downward out of the gun → magazine
   - rotates about an axis parallel to the barrel, offset from it → swing-out cylinder
   - visible only during reload, travelling in from outside the gun → round, rocket or loader
5. **Pick the type** from the parts found, the clip size the mod reports in `WeaponList` (a maximum of 1 means single shot), and reload sequence labels (a looped insert means a tube).
6. **Validate and label.** Same validator as hand-written cards.

**Validator rules** (plan's list, plus two): refuse on an unknown key or a bone the model lacks, listing the bones it has; warn on travel under about a unit, a peak one frame wide, an extent from a holster sequence; **warn when the part's parent moves during the measured sequence**; **warn when two parts claim the same role**.

**When the guess fails,** the weapon falls back to the ordinary reload button and says so once in the log and on the HUD. A wrong mechanism is worse than none: the MP5 throwing its magazine out every shot is the example to never repeat.

**Feel checklist — write this before touching code.** Seeded from behaviours the current code documents as hard-won; the owner confirms and extends it, and every item is checked in the headset before the old path is deleted:

- [ ] Stroke sound starts with the motion, at half travel, once per stroke
- [ ] Letting go at the back of a pump stroke still counts
- [ ] A braced off hand on the fore-end does not arm a stroke; a deliberate pull does
- [ ] After a round goes in, the hand must open and close again (or leave the gun) before it can rack
- [ ] A hand carrying a round is never read as working the action
- [ ] Empty self-loader: slide stays back; a short tug past the stop and release sends it home
- [ ] A slide nobody is holding never closes on its own
- [ ] Revolver: reload control opens; muzzle up dumps the cases; a wrist flick shuts it, but not while ammo is on its way in
- [ ] An open cylinder or an unchambered gun does not fire, and makes no dry click
- [ ] A removed magazine shrinks away over about 0.2 s and comes back seated the same way
- [ ] Holding reload for a second always performs an ordinary reload, on any weapon
- [ ] Waving the gun around with a hand on a part moves nothing

## Census results — the measurement half

Run on Sep 28, 2026 over **106 viewmodels in six sets** — every one present on this machine, including Opposing Force, Blue Shift and MMod. The generator and validator described above do not exist yet, so this is the measurement they will be built on: bones, sequences, mesh ownership per bone, and each bone's travel on all six axes across every sequence, decoded out of the `.mdl` animation data.

**Zero bone controllers in all 106.** The plan asserted this across seventeen models. It now holds across every viewmodel in stock Half-Life, both expansions and MMod. That route is closed for good.

| Set | Location | Viewmodels | Bone controllers |
| --- | --- | --- | --- |
| Valve SD | `Half-Life/valve/models` | 16 | 0 |
| Valve HD | `Half-Life/valve_hd/models` | 15 | 0 |
| Our test set | `XashVR/valve/models` | 18 | 0 |
| Opposing Force | `Half-Life/gearbox/models` | 24 | 0 |
| Blue Shift | `Half-Life/bshift/models` | 16 | 0 |
| MMod | `Half-Life MMod/HL1MMod/models` | 17 | 0 |

Opposing Force brings eight weapons stock Half-Life does not have: the M40A1 sniper rifle, the SAW, the shock rifle, the spore launcher, the displacer, the desert eagle, the knife and the pipe wrench, plus the barnacle grapple and the penguin. All but the pipe wrench carry at least one measurable moving part.

## The shipped part map reaches one rig in six

The strongest argument in this plan for generating cards rather than writing them is now measured. Our shipped default map names ten bones across seven weapons. **Valve HD and our test set score 10 out of 10. Valve SD, Opposing Force, Blue Shift and MMod each score 2** — the crossbow's `Slide` and `Clip Master`, and nothing else.

So on stock non-HD Half-Life, on both expansions, and on MMod, six of our seven carded weapons have no hand-drivable parts at all, and nothing in the log says so. Every headset session to date has run against HD-named rigs.

Travel is the peak range measured on the bone's dominant axis across every sequence; linear figures in units, rotational in degrees.

| Weapon / mapped bone | Valve SD | Valve HD | Our test | OpFor | Blue Shift | MMod |
| --- | --- | --- | --- | --- | --- | --- |
| `v_shotgun` / `Charger` | absent | Y 3.4 | Y 3.4 | absent | absent | absent |
| `v_9mmhandgun` / `Hands mesh 2` | absent | Z 3.3 | Z 3.3 | absent | absent | absent |
| `v_9mmar` / `clip` | absent | Z 10.1 | Z 10.1 | absent | absent | absent |
| `v_crossbow` / `Slide` | Y 10.0 | Y 10.0 | Y 9.9 | Y 10.0 | Y 10.0 | Y 10.0 |
| `v_crossbow` / `Clip Master` | Z 8.4 | Z 8.4 | Z 8.3 | Z 8.4 | Z 8.4 | Z 14.4 |
| `v_357` / `revolver` | absent | 63° | 63° | absent | absent | absent |
| `v_357` / `speed_loader` | absent | Z 66.4 | Z 66.4 | absent | absent | absent |
| `v_rpg` / `Rocket` | absent | Y 47.1 | Y 30.3 | absent | absent | absent |
| `v_grenade` / `ring` | absent | 128° | 66° | absent | absent | absent |
| `v_grenade` / `spoon` | absent | no travel | no travel | absent | absent | absent |

**The good news is that the other four are one family, not four.** Opposing Force, Blue Shift and MMod all inherit Valve's SD rigs and keep their bone names: `Speedload Master`, `cylinder_spin`, `Box02`, `Bone02`, `Dummy22`, `rocket_master`, `Bone01`, `Spinner`. One SD-family card set covers four of the six sets at once, which is a far better return than the per-weapon hand-tuning we have been doing.

It is a family, though, not a clone. Blue Shift's shotgun pump is `Bone02` travelling 2.14, where Valve SD's and Opposing Force's is `Bone01` travelling 4.93. MMod keeps the SD names and adds its own on top — `glauncher` and `Bone_clap` on the MP5, `bulletmaster` and `bullet02` on the revolver, `crossbow parent`, `Gauss`. Inheritance with per-model overrides is exactly the shape `base =` was drawn for.

`v_grenade` / `spoon` is worth a separate note: the bone exists in HD and in our test set but never moves in any sequence. A card naming it relies on declared travel, not measured travel, in both.

## What the census hands the generator

Five rules fall straight out of the measurements, and one open decision in this plan is answered by them.

1. **Gather travel across every sequence, never the one named after the action.** The shotgun's `pump` sequence moves nothing but finger bones. The pump's entire stroke lives in `shoot` and `shoot_big`. A generator that looked in the obviously-named sequence would find no pump on the one weapon we know works.
2. **Exclude the weapon body before ranking anything.** The largest mesh-owning mover is almost always the whole gun: HD `v_9mmar` / `carbine` at 60.7% of the mesh, Blue Shift `v_rpg` / `Dummy22` at 61.6%, OpFor `v_rpg` / `Dummy22` at 59.7%, MMod `v_egon` / `Gauss` at 52.6%. Each is a parentless bone carrying the model's body through its draw and idle animations. Rule: a root-parented bone owning more than roughly 40% of the mesh is the weapon.
3. **Exclude the camera rig.** MMod attaches a bone named `camera_bone` to 16 of its 17 viewmodels, and to none of the other 89 in this census. It owns 0.9–4.5% of the mesh — squarely in real-part territory — moves in most sequences, and travels 1.7 to 8.1 units. On MMod's revolver it travels 8.1 units, further than any genuine part on that weapon; on its shotgun it travels 2.3 against the real pump's 4.2. It is viewmodel camera animation, not a part, and a generator ranking by travel alone would pick it on nearly every MMod weapon. Any mod built on MMod's rigs inherits it.
4. **Mesh ownership separates parts from noise cleanly.** Across all 106 models, real parts own 3–10% of the vertices, finger bones about 1%, and the body 40–60%. This is the sharp signal the earlier auto-discovery attempt lacked — but rules 2 and 3 show it is not sufficient on its own.
5. **The generator must be allowed to return nothing.** `v_crowbar` has no drivable part in any set, OpFor's `v_pipe_wrench` has none, and HD `v_egon` and HD `v_gauss` have none while their SD counterparts do, at 90° and 344° of rotation. The same weapon legitimately has parts in one model set and none in another.

**The open decision this answers: cards cannot be keyed by gamedir alone.** Opposing Force and Blue Shift are their own gamedirs and are fine. The problem is `valve`: SD and HD are the same gamedir, `valve_hd` being an overlay mounted over `valve` when HD models are switched on. One `<gamedir>/vr/weapons.txt` cannot describe two rigs that need entirely different bone names. A card must be bound to the model it was measured from, not to the directory it was found in.

### The SD-family card, measured

These bones are shared by Valve SD, Opposing Force and Blue Shift, and inherited by MMod. Writing this one card set is the single largest coverage gain available — four of six model sets, from six rows. Figures are Valve SD's; the exceptions column names where a sibling differs and needs an override.

| Weapon | Bone | Axis | Travel | Appears in | Exceptions |
| --- | --- | --- | --- | --- | --- |
| `v_shotgun` | `Bone01` | X | 4.93 | `pump`, `shoot`, `shoot_big` | Blue Shift: `Bone02`, 2.14 |
| `v_9mmar` | `Bone12` | Y | 16.71 | `reload` | OpFor, Blue Shift: `Bone03`, 14.83 |
| `v_9mmhandgun` | `Box02` | Z | 21.10 | `reload`, `reload_noshot` | MMod adds `Box02.001` |
| `v_9mmhandgun` | `Bone02` | Y | 1.45 | `shoot`, `reload` | MMod: 2.43 |
| `v_357` | `Speedload Master` | Z | 62.85 | `reload` | identical in all four |
| `v_357` | `cylinder_spin` | Y | 30° | `fire1`, `reload` | MMod: X, 1.70 / 73° |
| `v_rpg` | `rocket_master` | Z | 60.02 | `reload`, `draw1`, `idle` | MMod: X, 35.95 |
| `v_grenade` | `Dummy23` | Y | 52.99 | `pinpull` | present in MMod, absent in OpFor and Blue Shift |
| `v_grenade` | `Bone04` | X | 139° | `pinpull` | OpFor, Blue Shift: 150° |

These are measured, not confirmed in a headset. Each names the bone whose travel and mesh share fit the role; which one a hand should actually reach for still needs a session.

### Opposing Force pays for the verb set

The plan cuts spinning barrels from v1 and keeps `dof2` for a bolt action. The census says both have real content waiting.

- **The bolt action exists.** OpFor's `v_m40a1` carries `m40a1_bolt` (X, 1.25 units, in `fire`, `firelastround` and `reload2`) and `m40a1_clip` (Z, 4.81, in `reload1` and `reload3`) as separate named bones. This is the one weapon in the whole census that needs `dof2`, and it is already rigged for it.
- **Two spinning barrels, not one.** OpFor's `v_displacer` / `spinner` turns 343.7° and SD `v_gauss` / `Spinner` turns 344.2°. If spinning barrels come back, they cover two weapons across three games.
- **The spore launcher loads visibly.** `v_spore_launcher` / `Sphere01` travels 58.3 units in a sequence named `reload_load` — a round being placed by hand, already animated.
- **The SAW and the shock rifle** both carry multiple moving parts (`Object05` at 17.5 units; `shock_rifle_body` at 19.8 with four telescoping segments), and neither fits the firearm types listed above. They are the first real test of whether the type list needs extending.

## Weapon mechanics beyond reloading

Racking and reloading are one part of handling a weapon. The rest builds on the same foundation — parts the hand moves, state owned by `vr_weapon` — in this order. Items 1–4 come with the weapon rework (step 2); 5–8 follow the campaign.

1. **Racking, done properly.** Every action type gets a stroke, and the stroke is the only way to chamber: slide (pull and release, or tug off the catch), pump (back and forward), charging handle (pull and let spring return), and a **two-axis bolt** (lift, pull back, push forward, drop). The M40A1 is already rigged for the bolt (`m40a1_bolt`, per the census). The shared code decides what a stroke does: ejects the chambered round if there is one, chambers the next if the magazine has one.
2. **Slide lock and release.** Empty self-loaders lock back (today's behaviour). Add a slide-release press on the dominant hand as a second way home, as real pistols have.
3. **Two hands on the weapon.** Stabilisation exists (`vr_twohand`). Add per-weapon grip points from the card (fore-end, magazine well, second grip), and make two-handed weapons lose accuracy when held one-handed rather than refusing to fire.
4. **Fingers.** Every Valve viewmodel carries finger bones driven by nothing. Curl them from the analog grip and trigger, wrap them around whatever part the hand is holding, and keep the index finger straight along the guard until the trigger is touched (trigger discipline). Client-only visual work; no netplay cost.
5. **Recoil as force, weight as lag.** Stop kicking the camera. Apply recoil as an impulse to the weapon and let it settle back to the hand; less with two hands, less when braced. Weapon mass, derived from the model's bounds so it works on uncarded mods, makes the weapon lag the hand slightly. Aim stays authoritative on the server: the muzzle pose sent is the settled pose the player sees.
6. **Haptic detents.** A part's travel is known exactly, so its events are too: the slide passing the catch, each cylinder chamber clicking by, the bolt stripping a round. Pulse the controller at those points instead of on fire alone.
7. **The weapon as a world object.** Today the weapon is a viewmodel drawn over the world: it passes through walls, casts no shadow and is lit differently. Draw it as a real entity in the hand: correct scale, occluded by the world, lit and shadowed with the room, and later **pushed back by walls** so the barrel cannot go through a door. That is also what other players see (step 7).
8. **No canned weapon animation, eventually.** Once every part is hand-driven and the weapon's pose is the hand, idle, draw and reload animations have nothing left to do. Keep them for uncarded weapons; retire them per weapon as its card covers every moving part.

The census has already found the content for three extensions the v1 types leave out: the two-axis bolt (M40A1), spinning barrels (Gauss, displacer) and a visibly placed round (spore launcher). Add them after v1 ships, in that order.

# Part F — The flagship campaigns (step 3)

## Finishing Half-Life, Opposing Force and Blue Shift

The three campaigns are mostly verification, not construction: the systems exist, and nobody has played them end to end. The method is the same for all three — play in order, record every session (pose recording, see *Testing*), and turn each failure into a replayable reproduction before fixing it. What follows is what is known or likely to need work, so the owner's sessions go to the right places.

**Mechanics every campaign needs**

| Mechanic | Where it bites | Today | Path |
| --- | --- | --- | --- |
| Train controls (`func_tracktrain`) | Half-Life *On A Rail*; Blue Shift's opening | Untested; world-reference code already follows the body when a train moves it | Use to take control, stick forward/back for throttle; later a physical lever you push, via the world-parts work |
| Mounted guns (`func_tank`) | *Surface Tension*, *We've Got Hostiles*, OpFor | Fire ray already falls back when the tank hides the viewmodel (`vr_openxr.c:5174`) | Aim with the dominant hand, not the head; draw two grips on the gun and let both hands hold it |
| Long jump | Xen, required | No handling | Map to a deliberate input (crouch + jump on the stick) and verify the server sees `IN_DUCK` + `IN_JUMP` in the same command; later, a physical lunge |
| Crouch-jumps and vents | Everywhere | Physical crouch exists | Check that physical crouch reaches `IN_DUCK` early enough for crouch-jumps; add a crouch toggle for seated players |
| Pushing and pulling crates | Half-Life, Blue Shift puzzles | Keyboard behaviour | Unchanged for now; grab-to-pull belongs to the props work |
| Swimming | Several maps | Stick only | Keep stick; add arm-stroke swimming later as an option |
| Scripted sequences and cameras | Intro tram, *Lambda Core*, endings | Untested | Never take the camera away in VR: override `trigger_camera` views with a fade or a world-anchored screen |
| Screen fades and shakes | Everywhere | User messages are already observed | Fades fade the world, not the head; shakes shake the world at reduced amplitude (comfort cvar) |
| Save and load in the headset | Always | Autosave and death reload exist | Verify quick save, quick load and the load menu with controllers only |

**Opposing Force specifics**

- **Ropes.** Opposing Force has climbable, swingable ropes. They are the natural next use of the two-handed ladder code: grab the rope with both hands, climb hand over hand, lean to swing, let go to jump.
- **Barnacle grapple.** Aim it with the hand that holds it; the pull should move the player, not the camera alone.
- **Night vision** replaces the flashlight; keep it on the same off-hand gesture.
- **Squad members** (medic, engineer) follow on use; touch-to-use must work on them.
- **Weapons.** Eight new weapons plus grapple and penguin. The census below measured their parts: the M40A1 bolt action is rigged for a two-axis bolt, the displacer spins like the Gauss, the spore launcher animates a round being placed, and the SAW and shock rifle don't fit any v1 firearm type yet. Default for anything without a card: ordinary reload button.

**Blue Shift specifics.** Mostly Half-Life's mechanics on the `bshift` branch. The opening tram and the Xen teleport sequences need the same camera and fade handling; crate-pushing puzzles need the same pull behaviour as Half-Life.

**Order.** Half-Life first. Opposing Force second, because ropes and the new weapons are real work. Blue Shift last, because it should mostly just work.

# Part G — Body and arms (step 4)

## Body and arm IK

The torso is already solved, and solved well. `VR_BodyUpdate` (`vr_openxr.c:7523`) estimates the neck pivot, the player's proportions (eye height and wingspan, as running percentiles) and torso yaw, and publishes neck, shoulder, chest and hip anchors in world space every frame, with a confidence value. Nothing is drawn from it yet. The path from here:

1. **Arms, analytically (step 4).** Two-bone IK from each shoulder anchor to the tracked wrist: upper arm and forearm lengths scale from the solved wingspan (`k_arm`), the elbow bends in the plane set by shoulder, hand and a pole vector (below and behind, biased outward when the hand is raised or crosses the chest). Clamp the elbow so it never enters the torso. Wrist twist is spread along the forearm so the mesh does not candy-wrap.
2. **Draw forearms first, upper arms second.** In the headset you see mostly forearms. Take the mesh from the HEV arms already welded into every viewmodel (today hidden by `r_vr_hide_arms`), driven by the IK instead of the animation, so arms always match the game's suit. Upper arms appear only in the lower periphery and can come after.
3. **Hand poses stay the controller's.** IK serves the hands; it never moves them. If the solve cannot reach, the arm stretches or fades at the elbow, never the hand — aim must not change.
4. **Torso and legs for the player: probably never.** Looking down at a solved torso with stick locomotion reads worse than nothing in most games. Keep it behind a cvar for the owner to judge; default off.
5. **Full body for other players (step 7).** Other players need the whole body. Send head and both hand poses in the player's entity state; each client runs the same solver on the player model (`player.mdl` rigs have standard Bip01 skeletons) with procedural legs from movement speed. Desktop players keep the stock animation.
6. **Uses beyond drawing.** Shoulder and hip anchors replace every hand-tuned holster offset (the review lists four numbers for the shoulder holster alone). Leaning and peeking fall out of the neck solve, and the body can drive the player's hitbox for fair deathmatch.

Done when looking down reads as your arms, the elbows never fight the aim, and none of the holster cvars are needed.

## The welded arms

Every weapon viewmodel carries Gordon's arms and hands as part of its mesh. In VR that is wrong twice over: the arms are posed by the weapon's animation rather than by the player, and they are modelled to be seen from behind, so from the side they cull away.

**Neither of the other two VR forks solved this.** HLVR has no arm-hiding code anywhere in its 764 source files: `VRHelper::UpdateViewEnt` attaches the whole viewmodel to the controller, arms included, and it ships `vr_hand_labcoat.mdl` and `vr_hand_hevsuit.mdl` only for the empty hand. Its weapon table is a hardcoded fifteen-entry `p_` → `v_` map covering stock Half-Life and nothing else. Lambda1VR is the same: `vr_hand_model` switches the entire viewmodel off rather than the arms, and `R_DrawHandModel` returns early whenever a weapon viewmodel exists, so with a gun in hand you get the welded arms.

Our `r_vr_hide_arms` already does the right thing — it skips a mesh at draw time when its texture is arm art, viewmodel only, so world models and other players keep theirs. It was the token list that was wrong.

**`r_vr_arm_textures` was `glove;sleeve;forearm`.** Those are the names Valve used on Gordon's gloves and nothing else. Measured against all 106 viewmodels, that list matched 24. Opposing Force, Blue Shift and MMod skin every arm with plain `hand.BMP` and `skin.BMP`, so it matched none of theirs — which made the feature look broken rather than unset, and is why the arms are still there today.

The default is now `hand.bmp;hands.bmp;skin.bmp;glove;sleeve;forearm`, which reaches 97 of 106. The `.bmp` on three of the tokens is deliberate: a bare `hand` also matches `handle.bmp`, `handleback.bmp` and `Pythonhandle.bmp`, which are grips — weapon art — and hiding those takes the grip off the gun. `r_vr_hide_arms` still defaults to 0.

The nine it does not reach are the hornet gun in every set, HD `v_gauss`, Opposing Force's barnacle grapple and its tongue tip, MMod's `v_hgun_puke`, and our own `v_hand_labcoat`. All but the last are organic weapons with no hand texture at all.

**One trap, worth recording because the guard is load-bearing.** Our own `vr_hand_hevsuit.mdl` is made entirely of `gordon_glove*` and `gordon_sleeve` textures — the token list matches 100% of it. It survives only because the mesh skip is gated on `RI.currententity == tr.viewent`, and the tracked hands are drawn as synthesised client-only entities rather than as the viewmodel. Anything that later routes a hand model through the viewmodel slot will erase it.

## Deriving the body meshes: shipped

Built and committed as `2f0dea90`, default-off. The token list above works, but it is authored data, and `PCVR_VR_DIRECTIONS.md` is explicit that authored data is a trap: it covers content we have looked at and nothing else. The census gives a derived signal that does not need a list.

**The signal.** Arm bones own a characteristic share of a viewmodel's vertices — finger bones about 1%, forearms 4–5%, hands 3% — while the weapon body sits on its own named bone at 40–60%. When an arm bone owns far more than its share, it is because the weapon is weighted to it. That is the whole test, and the models say so plainly: Opposing Force's crowbar puts 85% of its mesh on `Bip01 R Hand`, the pipe wrench 94%, the desert eagle 32%.

**The rule, measured across 105 viewmodels:** refuse the model entirely if any arm-tagged bone owns more than 15% of its vertices. That allows 86 and refuses 19, and the 19 are exactly the crowbars, the knife, the pipe wrench, the hand models, and the pistols and shotguns whose frame is weighted to a hand bone. No authoring, and it fails toward leaving the arms visible rather than deleting a weapon.

**Use both signals together.** Textures say *which* meshes are arm art; bones say *whether hiding them is safe on this model*. Each covers the other's failure mode — a mod that renames its textures still gets the bone test, and a model that weights its gun to a hand bone is refused whatever its textures are called.

**What shipped.** `R_StudioFindBodyMeshes()` runs once per model and answers both questions from the geometry: which meshes are arm art, and whether this model may be touched at all. It is cached per `studiohdr`, as `R_StudioFindParts` is, and the cache is invalidated when either cvar changes — so the ceiling can be retuned inside a headset without reloading the map.

One rule in the matcher carries the whole thing. A bone name matches as a case-insensitive substring, **but the following character may not be a letter.** Without that, `hand` swallows `Hands mesh 2` — which is the HD pistol's slide, not a hand — and the slide comes off the gun. A trailing digit is fine, so `Finger0` and `Bip01 R Arm2` still read as body.

**Verified rather than assumed.** A standalone harness carrying the same matcher and guard was compiled and run over all 106 viewmodels: 87 allowed, 19 refused, identical to the measurement above and on the same models. Compiling proves nothing about a heuristic.

| Cvar | Default | Does |
| --- | --- | --- |
| `r_vr_hide_arms` | `0` | The switch. Unchanged. |
| `r_vr_body_bones` | `finger;thumb;hand;arm;palm;clavicle` | Bone-name fragments read as the player's arms |
| `r_vr_body_guard` | `15` | Mesh-share percent at which a model is left alone |
| `r_vr_action_debug` | `0` | Logs which models were refused, and which bone did it |

|  | Files | Scope |
| --- | --- | --- |
| Analysis, cached per `studiohdr` as `R_StudioFindParts` is | `ref/gl/gl_studio.c` | 288 lines including the cvars |
| Declaration and registration | `ref/gl/gl_local.h`, `ref/gl/gl_opengl.c` | 4 lines |

Nothing under `engine/`, no game DLL, no protocol, no network message. Renderer-local, additive, default-off, and it degrades to current behaviour if the analysis refuses.

**The other half of the problem is separate.** The side-culling is single-sided viewmodel geometry, built to be seen from behind. Rendering the viewmodel pass two-sided fixes faces that are culled; it cannot add faces an artist never built, and which of those two Opposing Force suffers from has not been checked.

This also matters to the body IK above, which plans to drive the welded HEV arm mesh from the solver. That plan needs the arm meshes *identified*, not merely hidden — the same analysis produces both, so building it once serves both.

## IQM hands

The owner's hands are FBX models. The plan: load IQM natively, for **engine-owned models only** at first — the VR hands, later the arms and body. The server never precaches the hands and game code never draws them, so v1 needs no network change, no game-DLL change and no compatibility with client-DLL studio renderers. That keeps it to about four focused pieces of engine work.

**What IQM buys over today's `.mdl` hands.** Up to four bone weights per vertex, so knuckles bend instead of creasing; full-colour textures; normals and tangents for the renderer work in Part I; float precision; no GoldSrc vertex or bone limits; and a maintained Blender exporter. If the owner's Doom VR project already exports these hands to IQM, the same file should load here.

**The cheaper route, for comparison.** Our engine already renders weighted studio models: `R_StudioComputeSkinMatrix` blends up to four bones per vertex (`gl_studio.c:286`, used at `:2540` when a model carries `STUDIO_HAS_BONEWEIGHTS`). It also loads 32-bit TGA replacements from `materials/<model>/<texture>.tga` when materials are enabled (`gl_studio.c:4408`). So FBX → Blender → weighted SMD → a compiler that writes Xash's weighted format would give skinned hands with **no engine work**. The Valve `studiomdl` in `XashVR/tools` does not write that format; a Xash-aware compiler would (the PrimeXT toolchain is the one to evaluate). Worth an afternoon's test before committing, but IQM stays the target: it is the owner's existing pipeline and the format the body work will want.

**Phases**

| Phase | Work | Where | Done when |
| --- | --- | --- | --- |
| 1. Loader | Parse IQM v2 (header `INTERQUAKEMODEL`): meshes, vertex arrays (position, UV, normal, tangent, blend indices, blend weights), triangles, joints, poses, animations, frames. Validate every offset. Add a `mod_iqm` model type, loadable only through the VR layer in v1 | new `engine/common/mod_iqm.c`, `mod_local.h` | A hand file loads, and a malformed one is refused with a reason |
| 2. Renderer | Evaluate the joint pose, build skin matrices, skin on the CPU once per frame, draw that buffer for both eyes. Textures through `GL_LoadTexture` (TGA or PNG). Light it the way the weapon is lit — same light sample and dynamic lights — so hand and gun never disagree. New ref API entry point; bump `REF_API_VERSION` | new `ref/gl/gl_iqm.c`, `ref_api.h` | Hands draw in the headset, lit like the weapon |
| 3. Pose system | A sidecar file beside the model names the wrist and palm joints, the finger chains, scale and axes. The controller's grip pose is aligned to the declared palm joint — no pitch, yaw or pivot cvars. Named IQM animations (relaxed, fist, point, per-weapon grips) are blended from analog grip, trigger and thumb-touch. OpenXR hand tracking can later drive the joints directly | `vr_openxr.c` (hand section), new `<model>.vrhand` | Fingers curl with grip and trigger; the index finger lies along the guard until the trigger is touched; the hand pivots at the palm |
| 4. Integration | Draw IQM hands in place of `v_hand_hevsuit.mdl` when present; hide the arms welded into weapon viewmodels (census rules above). The dominant palm sits on the weapon's grip point; the off hand takes the pose of the part it holds. Fall back to `.mdl` hands if the IQM file is missing or refused | `VR_DrawHands`, cards (`grip_pose`) | A pistol sits in the IQM hand with the palm on the grip; removing the file brings back the old hands |
| 5. Later | IQM for arms and body (the two-bone IK above runs on IQM arm joints); other players' hands in netplay; and IQM as a general entity model. That last one is much bigger — server precache, attachments, hitboxes, events, and drawing outside client-DLL studio renderers — and only worth it if the platform needs it | — | — |

**Asset pipeline.** FBX into Blender; apply transforms; limit to four weights per vertex; name the animations; export with an IQM exporter; textures beside the file as TGA or PNG. Scale to Half-Life units (about an inch each): the stock HEV hand mesh measures about 6.7 units long, per the measurements in `vr_openxr.c`.

**Netplay.** None in v1: hands are drawn locally from local tracking. When other players' hands arrive in step 7, they use the same file, and a client without it falls back to `.mdl` hands.

**Lesson carried over.** The `.mdl` hands took many headset sessions of pitch and pivot offsets before the cause turned out to be the mesh's own rest pose. Declaring the palm joint in the sidecar, and aligning to it, is what stops that from happening again.

**Open:** where the FBX hands (and any existing IQM exports) live. They are not under `E:\XashWork`.

# Part H — VR mechanics (step 5)

## VR mechanics: throwing, climbing, touching the world

The principle from `PCVR_VR_DIRECTIONS.md` holds for all of these: find what the engine already knows and stop inferring it. Each mechanic follows the same netplay rule as reloading — the engine reports hands in `vrcmd_t`, and game code on the server decides.

| Mechanic | Today | Next | Later |
| --- | --- | --- | --- |
| **Throwing** | Grenades and satchels leave at hand speed; the arc is drawn | Send hand velocity and release in `vrcmd_t` so the server launches along the hand's actual direction and speed; release when the grip opens; pull the pin as a part (the census found the pin bones) | Spin from wrist angular velocity; place tripmines where the hand touches a wall; drop snarks from the hand |
| **Ladders** | Two hands on the rungs; weapon stows; step off at the top | Verify every ladder in the campaign | Same code for Opposing Force's ropes: hand over hand, lean to swing |
| **Mantling** | None | Grab a ledge edge with both hands and pull up: a ledge is a ladder with a different surface test | Climb over crates and pipes anywhere the hull fits |
| **Grabbing the world** | Use key only | `momentary_rot_button`, `momentary_door` and valve wheels store a 0-to-1 travel, like a slide: point the part driver at them and turn valves by hand | Levers, cranks, a train's throttle handle |
| **Touch to use** | Hand contact and a use trace exist (`VR_GetTouchContact`) | One "what is my hand inside" query against entity bounds and hitboxes; buttons and doors are pressed, not looked at | Same query answers "touching that scientist": lead NPCs by the hand |
| **Melee** | A hand-speed threshold anywhere | Use the crowbar's actual head: trace from where the head was last frame to where it is now, hit what it passes through, damage from its speed | Knife and pipe wrench (OpFor) with their own shapes; blocking a headcrab with a forearm |
| **Holsters** | Over-the-shoulder melee and flashlight, hand-tuned offsets | Place them from the body solve, no tuned numbers | Hip and chest holsters for pistols and grenades, if playtesting wants them |
| **Props** | None | Pick up small props (cans, gibs, crates you can carry in Half-Life) and throw them | GoldSrc has no rigid-body physics; a simple engine-side solver for carried props only |
| **Locomotion** | Stick, snap and smooth turn, teleport option, room scale, physical crouch | Long jump and crouch-jump (see campaigns) | Arm-swing walking and arm-stroke swimming as options |

Everything in the *Next* column should land within the campaign playthrough (step 3) or straight after it, because each is needed somewhere in the three games.

# Part I — The renderer (step 6)

## Expanding the renderer

The renderer is ours, and it is the safest place to be ambitious: everything it draws is local to one client, so no visual feature can break netplay. The limits are frame time and comfort. Two eyes at about 2500 × 2700 each (the Quest 2 target in the diagnostic log), at 72–90 Hz, is several times the pixel load of a flatscreen at 1080p. So the order is: buy headroom, move to a modern pipeline, then spend the headroom on light.

**Rules for all of it**

- Every feature sits behind a cvar, works in flatscreen too, and reports its cost in the diagnostic log. It defaults to on only after it is measured in the headset.
- Derive from what the engine already has — BSP light entities, lightmaps, texture names, model bounds — so it works on every map and mod without authoring. Optional packs (HD textures, material files) load per game directory and improve things; their absence must never break anything.
- Comfort beats spectacle: nothing may flicker between eyes, and nothing may adapt fast enough to be felt (exposure especially).

**R0 — headroom.**

- **Draw both eyes in one pass** with GL multiview (`GL_OVR_multiview2`). Today each eye is a full separate render. GoldSrc scenes are limited by draw calls and CPU, not the GPU, so this is the largest single performance win available.
- **Move world and model geometry into vertex buffers** instead of per-frame submission, through the existing `gl2_shim` path where it helps.
- **Foveated rendering** where the runtime exposes it; otherwise render the periphery at reduced resolution.
- **Frame timing in the log:** CPU and GPU time per frame and per pass, so every later feature has a number.

**R1 — a modern pipeline.** GLSL shaders for world, models, sprites and particles, replacing fixed-function state. Render into an offscreen linear floating-point target (RGBA16F), then one final pass writes the eye swapchain. The swapchain is plain `GL_RGBA8` today (`vr_openxr.c:9522`), so that final pass owns tone mapping and colour conversion.

**R2 — HDR.** Keep lightmap values above 1.0 instead of clamping them (GoldSrc's overbright range), so bright areas stay bright rather than flat. Tone map in the final pass. Exposure adapts slowly and within a clamped range, because fast adaptation is nauseating in a headset. Bloom stays subtle and is computed per eye with the same parameters.

**R3 — lighting.** The largest visible gain.

- **Per-pixel dynamic lights** for muzzle flashes, explosions and the flashlight, replacing the lightmap additions GoldSrc uses.
- **The flashlight as a projected spotlight with a shadow map,** cast from the off hand. Half-Life is dark and the flashlight is always in your hand, so this is the most noticed upgrade in VR.
- **Models lit per pixel** from the lightmap under them plus nearby dynamic lights, so NPCs and your own weapon sit in the room's light rather than a flat ambient.
- **Smoother static lighting:** bicubic lightmap filtering at once; later, optional runtime relighting from the map's own light entities, as the PrimeXT/XashXT renderers used by standalone Xash games do.
- **Shadow maps for a few important dynamic lights,** capped by budget.

**R4 — materials.** Optional per-texture material files matched by texture name: normal, specular and gloss maps, and parallax for floors and walls. HD texture packs load through the same mechanism. Xash's existing detail-texture support is the starting point.

**R5 — atmosphere.** Fog volumes, light shafts from the flashlight and strong lights, better water (reflection and refraction), upgraded particles and longer-lived decals. Each one is budgeted separately.

**R6 — ray tracing, as research only.** FWGS has an experimental Vulkan ray-traced renderer. Running it in VR means an OpenXR Vulkan path and twice the ray budget. Worth a spike after R3, not a commitment.

**Audio — the cheapest presence per unit of work.** Position the listener from the actual head (`VR_GetListener` already exists). Use HRTF spatialisation, occlusion traced through the BSP (a wall between you and a gunshot filters it rather than only attenuating it), and reverb from room size derived from the map's leaf data. Like the renderer, this is client-only and cannot affect netplay.

**Where this sits:** R0 can start any time, and should, because it pays for the rest. R1–R3 are step 6 of the work order. R4–R6 and audio follow in whatever order the owner prefers.

# Part J — Testing

## Testing with one PC and one tester

The owner's headset hours are the scarcest resource on the project. Anything a machine can check, a machine checks; the headset is for feel and for the campaign.

1. **Pose record and replay — build this first, during step 2.** Record head pose, both hand poses and all buttons per frame to a file (`vr_record`). On replay (`vr_replay`), feed those into the VR layer in place of OpenXR, drawing to the desktop window. The whole VR layer then runs without a headset: the owner records a reload once, and the coder replays it after every change.
2. **Log diffing.** Replay the same recording twice and diff the `vr_diag.log` output. Different logs mean something depends on frame timing. Keep a set of reference recordings (each weapon's reload, a ladder, a throw) and diff against their last good logs.
3. **`vr_weapon` tests without the engine.** The shared weapon code depends only on commands and state, so it can run in a small console program: feed it a scripted sequence of `vrcmd_t` and assert the result ("pull magazine, seat a new one, rack → chambered, clip 30").
4. **Card census.** A command-line run of the generator and validator over every viewmodel in `valve`, `gearbox` and `bshift`, printing each card and every warning. This is phase 0's census, and it doubles as the regression test for the generator.
5. **Netplay on one PC.** A dedicated server and two clients on localhost, each with its own port and config directory. One client replays a VR recording; the other is a desktop client. Add simulated lag and packet loss if the engine lacks them (GoldSrc had `fakelag` and `fakeloss`).
6. **Prediction diff.** On the client, log the predicted `vr_weapon` state for each command next to the state the server later confirms for it, and count mismatches. That turns "ammo counts jitter" from something felt into a number that must be zero.

# Part K — Open questions

## Open questions for the owner

None of these block steps 0–2. The coder should proceed on the stated default and flag it.

- [ ] **When the card generator fails on an unknown mod's weapon**, fall back to the ordinary reload button and say so? Default: yes.
- [ ] **A dropped part-used magazine loses its rounds** (today's patch keeps one in the chamber and drops the rest). Keep that, or return the rounds to the reserve? Default: keep; make it a server setting later.
- [ ] **Deathmatch: may VR players use the classic reload?** Default: server's choice, allowed.
- [ ] **Joining stock GoldSrc servers as a client** (protocol 48) still works today. Worth preserving, or free to break? Default: preserve while it costs nothing.
- [ ] **HEV forearms before full arm IK** — the review worries half a body reads worse than none. Default: build the forearms in step 4 and let the headset decide.
- [ ] **Which other mods get 64-bit DLLs in step 1**, beyond Half-Life, Opposing Force and Blue Shift? Default: none until the campaign playthrough is done.
- [ ] **Scope of the visuals work in step 6.** Owner to define.
