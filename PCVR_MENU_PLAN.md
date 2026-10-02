# PCVR menu plan — the controller is full

Status: **proposal, nothing built.** Written 2 Oct 2026, after the headset
backlog landed. Companion to `PCVR_WEAPONS_PLAN.md` and
`PCVR_PLATFORM_PLAN.md`; parts are lettered the same way.

---

## Why this is not a cosmetic job

Three measurements, taken from the tree on 2 Oct:

**1. There are 206 VR cvars.** `CVAR_DEFINE_AUTO` registrations matching
`vr_` or `r_vr_`, across `engine/client/vr/vr_openxr.c`,
`engine/server/sv_pmove.c` and `ref/gl/gl_studio.c`. Every one of them is
reachable only by typing at a console you cannot see while wearing a
headset. `valve\vrbinds.cfg` is 200 lines of hand-tuned numbers that exists
*because* there is nowhere else to put them.

**2. The Quest 2 Touch controller is completely saturated.** The suggested
bindings at `engine/client/vr/vr_openxr.c:12161` assign **every physical
input the controller has**:

| Input | Bound to |
| --- | --- |
| L stick axes | MOVE |
| R stick axes | TURN |
| R trigger (analog) | TRIGGER + ATTACK |
| R squeeze (analog) | GRIPFORCE + ATTACK2 |
| L squeeze (analog) | OFFGRIPFORCE + OFFGRIP |
| R A | JUMP |
| R B | CROUCH |
| L X | RELOAD |
| L Y | FLASHLIGHT |
| R stick click | NEXTWEAP |
| L stick click | PREVWEAP |
| L menu | MENU |
| R thumbrest | THUMBREST (touch) |
| L thumbrest | OFFTHUMBREST (touch) |

**There is not one free input left.** The right-hand menu button is reserved
by the runtime and cannot be taken. 19 `VRA_` actions are already mapped onto
14 physical controls by doubling trigger and squeeze up.

**3. The mechanics built on 1 Oct have no controller home at all.** Holster
*assignment* is a console command (`vr_holster hip`). The hold-sim A/B that
gates Part F is a console command. The rear-view target is a console command.
The nine-check launcher put them on **F1-F12 of a keyboard you cannot see**,
which is a workaround, not an answer.

So: the menu is not "way further down the road as long as the Touch
controllers do what we need". The Touch controllers **already do not**. The
menu is the input budget's only escape valve, and every mechanic added from
here makes that worse.

---

## What already exists — do not rebuild these

- **A world-leashed 2D panel in the headset.** `VR_Begin2D` / `VR_End2D`,
  `vr_menu_leash`, `vr_menu_yaw` / `vr_menu_pitch`. The panel drags its anchor
  rather than letting itself leave your eye.
- **A controller pointer that drives the existing menu.** Around
  `vr_openxr.c:10820`: the dominant hand's aim angles are expressed against
  the panel rect and handed to `UI_MouseMove`, with the trigger
  edge-triggered into `UI_KeyEvent( K_MOUSE1 )`. Pointing at the stock menu
  and clicking it **works today**.
- **The HUD and menu composite into each eye.** `cl_view.c:751` calls
  `CL_DrawHUD`, `UI_UpdateMenu` and `Con_DrawConsole` inside the eye loop
  while the FBO is bound, gated on `vr_hud`.
- **A paged settings menu built from a text file.** `3rdparty/mainui`
  (`CFGScript.cpp` plus `menus/dynamic/ScriptMenu.cpp`) parses a `.scr` file
  into checkboxes (`T_BOOL`), sliders with min/max (`T_NUMBER`), dropdowns
  (`T_LIST`) and text fields (`T_STRING`), splits them across pages, and
  saves back. **This is most of Part B already written by someone else.**

## The one real obstacle

`3rdparty/mainui` is a **git submodule of `github.com/FWGS/mainui_cpp`**, at
`continuous-549-g718371c`. It registers **no console commands** — the script
menu is reachable only through `UI_AdvServerOptions_Menu` and
`UI_AdvUserOptions_Menu`, which hang off Create Server and player setup, not
off the pause menu.

**There is therefore no zero-fork path to a settings page.** Any entry point
means a third fork to track alongside the engine and the game DLLs. That cost
is small but it is real, and it should be decided deliberately rather than
discovered halfway in.

---

## Part A — Decide the chord, before anything is built

Everything below needs one physical gesture to summon it, and there is no
button left to spend. The options, in the order I would try them:

- **A-01 — Long-press the left menu button.** Short press keeps the stock
  pause menu, long press opens ours. Costs nothing, loses nothing. Check the
  other profiles first: the Index block at `vr_openxr.c:12206` maps MENU to
  `system/click`, which runtimes often reserve.
- **A-02 — Look at your wrist.** Turn the off hand palm-up toward your face
  and the panel appears on the forearm. No button at all, and it pairs with
  the wrist HEV display already on Part I's list. Needs a dot-product gate
  with hysteresis so it does not flicker at the threshold.
- **A-03 — Off-hand thumbrest plus stick.** `VRA_OFFTHUMBREST` is already
  wired and is currently read by almost nothing. A radial that opens while
  the thumb rests and the stick deflects is the standard VR idiom, and it
  costs no button because a thumb on the rest is not a press.

**A-02 and A-03 are complementary, not alternatives** — wrist for settings,
thumbrest radial for in-combat verbs. A-01 is the cheap fallback if both
prove fiddly in practice.

## Part B — The 206 cvars, generated

- **B-01 — `tools/vrcard/scrgen.py`.** Walk the three files for
  `CVAR_DEFINE_AUTO`, read the name, the default, and **the description
  string that is already there**, and emit a `.scr`. Type inference: a
  `"0"`/`"1"` default with an on/off description gives `T_BOOL`; numeric
  gives `T_NUMBER`; anything else `T_STRING`. Ranges and page grouping come
  from a small hand-written override table, not from guessing.
- **B-02 — Ship `valve/vr.scr`, regenerated in the build.** A cvar added
  without a menu entry then becomes a build-visible omission rather than a
  silent one. Same discipline as `audit_build_refs.py`.
- **B-03 — The entry point.** This is the mainui fork: one page-selector
  entry and one `SetScriptConfig( "vr.scr", false )`. Smallest diff that
  works, so it survives a rebase.

**B makes all 206 adjustable in-headset for roughly one Python file.** It
does not solve Part A, and it is useless mid-combat — it is the sit-down-and-
tune surface, which is exactly what `vrbinds.cfg` is being abused as today.

## Part C — The radial command surface

This is the part that actually fixes the saturated controller, and the part
that has to be built **general**, per the engine rules.

- **C-01 — `engine/client/vr/vr_radial.c` / `.h`.** A radial menu with no
  knowledge of what is on it. Entries come from a text file in the gamedir —
  label, icon, console command, optional submenu — so a mod ships its own and
  the engine special-cases nobody. Additive and default-off (`vr_radial 0`).
- **C-02 — Name it for what it does.** It is a *command surface*, not a
  weapon wheel and not a holster picker. The second caller is not
  hypothetical here: holster assign and draw, weapon selection, the hold-sim
  A/B, the HEV wrist display and the rear-view toggle all want it on day one,
  and each is one line of a text file instead of a case in a switch.
- **C-03 — Draw it in 3D, not as a 2D blit.** The existing panel is flat and
  leashed because it is `UI_UpdateMenu` composited into the eye. A radial
  anchored to the off hand should be a real object at a real depth — it reads
  better, and it sidesteps the whole eye-space-versus-world-space argument.
- **C-04 — Selection without a click.** Stick deflection picks the wedge,
  release commits. The trigger is busy being the gun.

## Part D — Remapping the 19 actions

Once Part B exists, a bindings page is mostly free: the OpenXR action set is
a table and the suggested bindings at `vr_openxr.c:12161` are a literal
array. Exposing it means rebinding at runtime rather than once at
`xrSuggestInteractionProfileBindings` time, which OpenXR permits but the
current code does not do.

**Lower priority than it looks.** With zero free inputs, remapping only
shuffles the same shortage around. Part C creates capacity; Part D merely
redistributes it. C first.

## Part E — The fork decision

| | Cost | Gets you |
| --- | --- | --- |
| **Fork `mainui_cpp`** | A third repo to track and push to `pcvr` | Real widgets, real pages, already written |
| **Engine-side UI in `vr/`** | Write every widget from scratch | Total control, 3D-native, no submodule |
| **Both** | The fork, but kept to a few lines | B through mainui, C in the engine |

**Recommendation: the third.** The fork stays small enough to rebase — one
entry point and one `SetScriptConfig` call — and buys a settings page we
would otherwise spend weeks on. The radial, which is the genuinely novel
part, which has to be 3D, and which has to be general, lives in our own tree
where the engine rules apply and nobody rebases it out from under us.

---

## Order, and what it is worth

1. **Part A.** One decision, no code. Everything else is blocked on it.
2. **Part C.** The capacity problem. Holster assignment stops being a console
   command, and the next mechanic has somewhere to go.
3. **Part B.** The tuning problem. `vrbinds.cfg` stops being a user interface.
4. **Part D.** Only once C has created something worth redistributing.

**Not scheduled against the weapons plan or the platform plan.** Step 3 of
the platform plan is "owner plays Half-Life start to finish", and that needs
none of this — F1-F12 and `vrbinds.cfg` will carry a playthrough. But every
mechanic added *after* that playthrough needs Part C to exist, and Part C is
blocked on Part A being answered.
