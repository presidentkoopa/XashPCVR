# Xash3D FWGS — PCVR Fork: Work Log

**Goal:** Engine-level OpenXR VR layer for Xash3D FWGS on desktop Windows, usable across multiple
GoldSrc mods (Half-Life, TFC, Ricochet, They Hunger). Personal / LAN use, no distribution.

**Working branch:** `pcvr-openxr` (branched from mainline FWGS `2f24dfe8`)

**Reference repos (read-only, do not modify):**

| Path | What it is |
|---|---|
| `E:\XashFWGS` | This repo. Mainline Xash3D FWGS (GPLv3, C, waf build). |
| `E:\Lambda1VR` | DrBeef/Team Beef. Xash3D-FWGS + OpenXR, **Android/Quest only**. Source of the OpenXR frame loop. |
| `E:\HalfLifeVR` | Max Makes Mods Half-Life:VR. A GoldSrc **game-DLL** mod. Source of 6DoF interaction logic. |
| `E:\OpenXR-SDK` | Khronos OpenXR SDK (headers + loader source). |
| `E:\deps\SDL2-2.32.10` | SDL2 VC dev package (required for Windows build). |

---

## Build commands

```bash
cd /e/XashFWGS
./waf.bat configure -T release -s /e/deps/SDL2-2.32.10
./waf.bat build
```

Artifacts: `build/engine/xash.dll`, `build/ref/gl/ref_gl.dll`

---

## Environment (this machine)

| Thing | Status |
|---|---|
| Visual Studio | `C:\Program Files\Microsoft Visual Studio\18\Community` (MSVC, v143-era) |
| cmake | `.../Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe` (not on PATH) |
| ninja | `.../Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe` (not on PATH) |
| Python | 3.12.10 (waf works) |
| Steam libraries | `C:\Program Files (x86)\Steam`, `D:\SteamLibrary` |
| Half-Life (appid 70) | Installed (D:\SteamLibrary) |
| Half-Life: VR Mod (appid 1908720) | Installed (D:\SteamLibrary) |

---

## FINDING 001 — Baseline builds clean

Mainline FWGS compiles out of the box once SDL2 is supplied: **596/596 targets, ~30s**.
Fast iteration loop confirmed. SDL2 was the only missing dependency (waf `-s/--sdl2` flag,
explicitly "required for Windows").

## FINDING 002 — Engine builds 32-bit (x86)

`build/engine/xash.dll` and `build/ref/gl/ref_gl.dll` are both **PE32 / Intel i386**.
waf auto-selected `E:\deps\SDL2-2.32.10\lib\x86`.

This is the correct default: GoldSrc mods ship **32-bit** `client.dll` / `hl.dll` game DLLs.
A 64-bit engine cannot load them — it would require rebuilding every mod from source.

## FINDING 003 — **CRITICAL** — SteamVR has no 32-bit OpenXR runtime

```
C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json   ← EXISTS
C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win32.json   ← DOES NOT EXIST
```

**SteamVR's OpenXR runtime is 64-bit only.** A 32-bit engine cannot use SteamVR via OpenXR.

This is the central architectural tension of the project:

- **32-bit engine** → can load existing mod game DLLs (HL, TFC, Ricochet, They Hunger as shipped)
  → but **cannot** use SteamVR's OpenXR runtime.
- **64-bit engine** → can use SteamVR OpenXR
  → but **cannot** load any stock mod DLL; every mod must be rebuilt from source as 64-bit.

## FINDING 004 — Active OpenXR runtime is VirtualDesktopXR, and it ships 32-bit

Registry:
```
HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime
  = C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json      (64-bit)
HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime
  = C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.json   (32-bit)
```

**VDXR provides a 32-bit OpenXR runtime** (`virtualdesktop-openxr-32.dll`) where SteamVR does not.
This potentially resolves FINDING 003 in favour of staying 32-bit — pending verification that
VDXR supports the OpenGL graphics binding (see OPEN QUESTION 1).

## FINDING 005 — Renderer is a clean module boundary

`engine/ref_api.h` defines `ref_interface_s`:
- `GL_SetupAttributes` (~:537)
- `R_BeginFrame` (~:543)
- `R_RenderFrame`
- `R_EndFrame` (~:545)

Renderer ships as a separate loadable module (`ref_gl.dll`; also `ref_soft`, `ref_null`).
`ref/gl` is 17 `.c` files. This is the intended stereo insertion point — no engine-internals hacking.

## FINDING 006 — Lambda1VR's OpenXR frame loop is complete and standard

`E:\Lambda1VR\Projects\Android\jni\src\Lambda1VR\TBXR_Common.c` (2019 lines) contains the full
standard OpenXR lifecycle:

| Call | Line |
|---|---|
| `xrCreateInstance` | 1604 |
| `xrCreateSession` | 1367 |
| `xrCreateSwapchain` | 451 |
| `xrWaitFrame` | 1809 |
| `xrBeginFrame` | 1819 |
| `xrLocateViews` | 1898 |
| `xrEndFrame` | 2016 |

Companion files: `OpenXrInput.c` (568 lines, action-set input), `L1VR_SurfaceView.c` (861 lines,
Android surface + `Host_Main` bridge).

This is real OpenXR — the same API SteamVR/VDXR implement on desktop. The port work is
**swapping the graphics binding** (`XrGraphicsBindingOpenGLESAndroidKHR` →
`XrGraphicsBindingOpenGLWin32KHR`) and re-hosting the loop, not inventing VR support.

`VrInputDefault.c` / `VrInputAlt.c` / `VrInputAlt2.c` / `VrInputOne.c` are comfort-preset control
scheme *variants* (near-duplicate headers — `VrInputAlt.c` still says "VrInputDefault.c"), not
competing architectures. Pick one, ignore the rest.

## FINDING 007 — **RISK RESOLVED** — direct OpenGL submission works, headset live

Built a 32-bit OpenXR probe (`vr/probe/xr_probe.c`, `vr/probe/build_probe.bat`) and ran it
against the live runtime. Full end-to-end success:

```
[runtime] name    : VirtualDesktopXR
[runtime] version : 1.0.10
[system]  name    : Oculus Quest2      (orientation: yes, position: yes)

[stereo views] 2
    eye 0: recommended 2496x2688 (max 16384x16384), samples 1
    eye 1: recommended 2496x2688 (max 16384x16384), samples 1

XR_KHR_opengl_enable : YES (v10)
[GL requirements] min OpenGL 4.0  max 5.0
  => GL binding is fully functional on this runtime.
```

**OPEN QUESTION 1 is answered: we do NOT need D3D11 interop.** Direct OpenGL submission
via `XR_KHR_opengl_enable` is supported. D3D11/D3D12/Vulkan also available as fallbacks.
31 extensions total, including `XR_EXT_hand_tracking` and `XR_HTCX_vive_tracker_interaction`.

## FINDING 008 — Must request OpenXR API version **1.0**, not `XR_CURRENT_API_VERSION`

First probe run failed with `XR_ERROR_API_VERSION_UNSUPPORTED (-4)`. Cause: the Khronos SDK
headers define `XR_CURRENT_API_VERSION = 1.1.62`, but VDXR's runtime implements **1.0** only.

```c
ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;   /* correct */
/* NOT XR_CURRENT_API_VERSION -- rejected with -4 */
```

The probe now negotiates downward (tries 1.0, then 1.1) and reports which succeeded.
**Engine code must request 1.0.**

## FINDING 009 — GL context must be **>= 4.0**; use a COMPATIBILITY profile

VDXR reports `min OpenGL 4.0, max 5.0`. Xash's `ref_gl` is legacy GoldSrc-style rendering
(fixed-function / GL 1.x-2.x idioms). These are reconcilable only via a **4.x compatibility
profile** context — which satisfies OpenXR's version floor while keeping legacy GL calls valid.

Action: `engine/platform/sdl2` GL context creation (and `GL_SetupAttributes` in `ref/gl`)
must request `SDL_GL_CONTEXT_PROFILE_COMPATIBILITY` with major >= 4. **Verify this does not
regress normal flatscreen rendering.**

## FINDING 010 — Integration map: exactly where stereo goes

Traced the full frame path. The engine already renders in a **multi-view loop**, which is
almost exactly the shape stereo needs.

`engine/client/cl_view.c:387` — `V_RenderView()`:

```c
do {
    clgame.dllFuncs.pfnCalcRefdef( &rp );   // <-- THE MOD's client.dll computes the view
    V_GetRefParams( &rp, &rvp );
    V_RefApplyOverview( &rvp );
    V_ApplyRefUnderwater( &rvp );
    GL_RenderFrame( &rvp );                 // <-- renders one view
    S_UpdateFrame( &rvp );
    viewnum++;
} while( rp.nextView );                     // engine ALREADY supports N views/frame
```

### Why this makes VR mod-agnostic (the key architectural insight)

`pfnCalcRefdef` is the **mod's** client.dll computing where the camera is. We do **not** touch it.
We let the mod compute its normal view, then **override `rvp` in engine code afterwards** with the
per-eye pose from OpenXR. Because the override lives in `cl_view.c` (engine, compiled once), it
applies to **any** mod's client.dll — Half-Life, TFC, Ricochet, They Hunger — with no per-mod work.

### The struct we override — `common/ref_params.h:97` `ref_viewpass_t`

```c
typedef struct ref_viewpass_s {
    int   viewport[4];   // <- set to OpenXR eye swapchain size (2496x2688 here)
    vec3_t vieworigin;   // <- HMD position + per-eye IPD offset
    vec3_t viewangles;   // <- HMD orientation
    int   viewentity;
    float fov_x, fov_y;  // <- per-eye FOV (see asymmetry note below)
    int   flags;
} ref_viewpass_t;
```

Everything stereo needs is already in this struct. No new plumbing to the renderer required.

### Frame boundaries

| Hook | Location | VR use |
|---|---|---|
| `R_BeginFrame` | called `engine/client/cl_view.c:349` | `xrWaitFrame` / `xrBeginFrame` |
| `GL_RenderFrame(&rvp)` | `engine/client/cl_view.c:417` → `ref/gl/gl_rmain.c:1076` `R_RenderFrame` | render one eye into its swapchain image |
| `R_EndFrame` | called `engine/client/cl_view.c:576` | `xrEndFrame` (submit both layers) |

### Asymmetric FOV — small, contained fix

`ref/gl/gl_rmain.c:373` `R_SetupProjectionMatrix()` currently forces a **symmetric** frustum:

```c
GLfloat yMax = zNear * tan( RI.rvp.fov_y * M_PI_F / 360.0f );
GLfloat yMin = -yMax;                                  /* forced symmetric */
GLfloat xMax = zNear * tan( RI.rvp.fov_x * M_PI_F / 360.0f );
GLfloat xMin = -xMax;                                  /* forced symmetric */
Matrix4x4_CreateProjection( m, xMax, xMin, yMax, yMin, zNear, zFar );
```

**Good news:** `Matrix4x4_CreateProjection` already accepts independent left/right/top/bottom —
the symmetry is imposed by the *caller*, not the matrix builder. VR needs only:

```c
xMin = zNear * tan( fov.angleLeft );    xMax = zNear * tan( fov.angleRight );
yMin = zNear * tan( fov.angleDown );    yMax = zNear * tan( fov.angleUp   );
```

fed from `XrFovf`. Note `zNear` is hardcoded `4.0f` (HL units; ~0.1 m at 39.37 u/m) — may want
reducing so near-face hands don't clip.

### GL context creation

`engine/platform/sdl2/vid_sdl2.c:725` `SDL_GL_CreateContext( host.hWnd )`, with attribute
plumbing at `:838`. Must be raised to a **4.x compatibility** profile (FINDING 009), and this is
also where we get the Win32 `HDC`/`HGLRC` for `XrGraphicsBindingOpenGLWin32KHR`
(via `SDL_GetWindowWMInfo`).

## FINDING 011 — OpenVR vs OpenXR: why HLVR works on Steam and we can't copy it

HLVR uses **OpenVR**, not OpenXR — an entirely different, older, Valve-native API.
Verified: `vr::VR_Init()` at `E:\HalfLifeVR\src\cl_dll\VRHelper.cpp:359`,
`vr::IVRSystem*` at `:1487`, bundled `openvr_api.dll` is **x86**, and there are
**zero** OpenXR call sites in the entire HLVR tree.

SteamVR ships `bin/win32/openvr_api.dll` and a 32-bit `vrclient.dll`:

| API | SteamVR 32-bit | SteamVR 64-bit |
|---|---|---|
| **OpenVR** (legacy, Valve-native) | YES | YES |
| **OpenXR** (modern standard) | **NO** | YES |

So FINDING 003 applies to **OpenXR only**. HLVR (32-bit + OpenVR) runs on SteamVR fine.

**Decision: stay on OpenXR.** Rationale:
- Proven working on this machine's hardware today (FINDING 007).
- Lambda1VR's ~2000-line frame loop is OpenXR; going OpenVR discards that reference,
  and HLVR's OpenVR code is entangled with its `opengl32`-hooking scaffolding.
- `vr_openxr.h` is deliberately backend-agnostic (`VR_Init` / `VR_BeginFrame` /
  `VR_BeginEye` / `VR_EndEye` / `VR_EndFrame`). An OpenVR backend later means
  implementing those same functions, not redoing the engine integration.

**Known cost:** a 32-bit OpenXR build cannot drive a native-SteamVR-only headset
(Index, Vive). Quest/Pico via Virtual Desktop are fine. Revisit if a player has one.

## FINDING 012 — **BUG FOUND AND FIXED** — mainui struct-size mismatch

Adversarial recon caught a real defect introduced by extending `ref_viewpass_t`.

`menu.dll` (mainui) is built against its **own private copy** of the struct at
`3rdparty/mainui/sdk_includes/common/ref_params.h:97`, which lacks the VR fields.
`pfnRenderScene` in `engine/client/dll_int/cl_gameui.c` did a whole-struct
assignment `copy = *rvp;` using the **engine's larger** definition — reading past
the end of the caller's smaller struct and leaving `vr_active` as stack garbage.
Non-zero garbage would enable the asymmetric-frustum path with junk tangents on the
menu player-model preview.

Fixed by copying only the prefix the caller owns and zeroing the VR fields.
**Lesson: `ref_viewpass_t` is not as private as it looks — mainui has a copy.**

## FINDING 013 — Corrections to the integration map (FINDING 010)

Recon re-verified FINDING 010 against source and corrected it. Retained here because
these are the traps that would have cost real debugging time:

- **`ref_interface_t` has NO `R_RenderFrame` member.** The engine's only scene-render
  slot is `GL_RenderFrame` (`engine/ref_api.h:630`), wired at `ref/gl/gl_context.c:558`
  to `ref/gl/gl_rmain.c:1076`. `R_RenderScene` (`ref_api.h:544`) is exported but the
  engine **never calls it**. The engine-side wrapper
  `engine/client/dll_int/ref_common.c:172` is the sole caller — it catches both
  `cl_view.c:417` and the menu path `cl_gameui.c:835`. **Hook there, not in cl_view.c.**

- **`R_SetupGL` y-flips the viewport against the WINDOW height**
  (`gl_rmain.c:547-550`, effectively `pglViewport(x, H - y - h, w, h)`). Our eye FBO is
  2496x2688, not the window size, so this flip is wrong for VR. Precedent exists: the
  `RF_DRAW_CUBEMAP` branch at `:556-559` already bypasses the flip and uses
  `RI.rvp.viewport` verbatim. VR needs the same treatment.

- **Culling frustum is separate from the projection matrix.** `R_SetupFrustum`
  (`gl_rmain.c:344`) builds it via `GL_FrustumInitProj(..., fov_x, fov_y)` at `:364`,
  which hardcodes symmetry (`gl_frustum.c:37/40/44`). Our asymmetric projection fix does
  **not** feed it, so culling will be subtly wrong per eye until this is addressed too.

- **Per-eye double-fire hazards inside `R_RenderFrame`:**
  - `R_RunViewmodelEvents()` (`gl_rmain.c:1119`) would fire twice per frame.
  - `tr.realframecount++` (`:1122`, also `:1112`) is the same-frame dedupe key for
    player gait (`gl_studio.c:2852`) and is exported to mods (`:398`) — it **must**
    increment exactly once per frame, not once per eye.
  - `tr.frametime` (`:965-967`) drives particle integration (`gl_rpart.c:102`) and
    tracers (`:242`) — double-stepping would run effects at 2x speed.
  - `tr.framecount` (per-surface `visframe`) **must** keep incrementing per eye.
  - Beams are unaffected: `gl_beams.c:1240` recomputes its own delta.

---

## Test setup (how to run it)

Assembled at `E:\XashVR`. Xash needs **no Steam and no SteamVR** — it reads the
Half-Life `valve/` assets directly via `-rodir`, read-only, writing configs and saves
into `E:\XashVR` instead. The Steam install is never modified.

Contents: `xash3d.exe`, `xash.dll`, `ref_gl.dll`, `filesystem_stdio.dll`, `menu.dll`,
`vgui_support.dll`, `SDL2.dll`, `openxr_loader.dll`, plus **`vgui.dll` copied from the
Half-Life root** (required by HL's `client.dll`; not finding it is a hard startup error).

```
E:\XashVR\run.bat
```
which runs:
```
xash3d.exe -rodir "D:\SteamLibrary\steamapps\common\Half-Life" -game valve -console -dev 2
```

**Status: WORKS.** Engine boots, loads HL's real `cl_dlls/client.dll` and `dlls/hl.dll`
(proving 32-bit mod-DLL loading), `ref_gl` initializes on the RTX 3080 Ti, no errors.
Reaches the intro tram sequence.

### To test VR

VR activates only when a headset is actually present. **Start Virtual Desktop
Streamer and connect the headset first**, then run `run.bat`. Without a headset the
log shows `VR: no HMD present, running flatscreen` and the game runs flat — that is
the intended graceful fallback, not a failure.

## FINDING 014 — VR wired into the frame loop; graceful fallback confirmed

`VR_Init()` now runs at the top of `R_Init()` (`engine/client/dll_int/ref_common.c`),
before any GL context exists, so `GL_SetupAttributes` can request a context version the
runtime accepts. `VR_InitSession()` runs after the renderer succeeds, when the GL
context is current and can supply the Win32 `HDC`/`HGLRC`.

Verified live without a headset connected:
```
VR: runtime VirtualDesktopXR 1.0.10
VR: no HMD present, running flatscreen
```
Instance creation succeeds, `xrGetSystem` correctly reports no HMD, and the engine
falls back cleanly. **This is also the flatscreen-player code path** — the same binary
serves VR and non-VR players, which is what makes VR/flatscreen LAN crossplay free.

### Changes made to wire it up

| File | Change |
|---|---|
| `engine/client/dll_int/ref_common.c` | `VR_Init()` early in `R_Init()`; `VR_InitSession()` after renderer load |
| `engine/platform/sdl2/vid_sdl2.c` | engine overrides `ref_gl`'s GL attributes to force a 4.x **compatibility** context when VR is available |
| `engine/client/cl_view.c` | `V_RenderView()` gains a stereo path: call the mod's `pfnCalcRefdef` **once**, then render the scene once per eye |
| `ref/gl/gl_rmain.c` | VR viewport bypasses the window-height y-flip (FINDING 013); viewmodel events + `tr.realframecount` + `tr.frametime` gated to `vr_eye == 0` |
| `common/ref_params.h` | added `vr_eye` index so the renderer can distinguish first eye from second |
| `engine/client/vr/vr_openxr.c` | eye position emitted as offset from head centre; conservative symmetric FOV published for culling |

### Design decisions worth remembering

- **Eye anchoring.** OpenXR poses are in tracking space, not world space. `VR_BeginEye`
  emits the eye position as an **offset from head centre** (essentially the IPD half-
  offset); `cl_view.c` adds the mod's computed view origin. This yields correct stereo
  and full head **orientation** tracking without pretending to have solved room-scale
  **positional** tracking — that is priority item 2 and needs real play-space↔world
  reconciliation (port from HLVR's `vr_ClientOriginOffset` / `vr_lastHMDOffset`).

- **Culling frustum is conservative, not exact.** `R_SetupFrustum` builds a *symmetric*
  frustum from `fov_x/fov_y`, independent of the projection matrix. Building it from the
  true asymmetric angles would clip the wider side, so `VR_BeginEye` publishes twice the
  **larger** half-angle — guaranteed to contain the real frustum. Over-inclusive (draws a
  little extra) is safe; under-inclusive would pop geometry at the edges.

## FINDING 015 — **Leaked OpenXR sessions**: the failure mode that wastes the most time

Symptom: headset goes black while **sound and controls still work**. Easy to misread as a
rendering bug. Actual cause: `xrCreateSwapchain` fails with `XR_ERROR_RUNTIME_FAILURE`
because leftover `xash3d.exe` processes are still holding OpenXR sessions and the runtime
has a session limit. Five had accumulated before this was spotted.

Three independent causes, all now fixed:

1. **`VR_Shutdown()` was never called anywhere.** It existed but had zero call sites, so the
   session was never released on exit. Now called from `R_Shutdown()`
   (`engine/client/dll_int/ref_common.c`), before the GL context is destroyed, plus an
   `atexit()` backstop for paths that never reach it (`Sys_Error`, crash dialog, `Host_Abort`).

2. **`-console` keeps the process alive.** That flag opens a console whose
   "Press Enter to continue..." exit prompt blocks forever, so the process — and its session —
   survives the game closing. **Removed from run.bat.** `-log` still captures everything.
   Note such a process reports as running but `taskkill /F` refuses it
   ("no running instance of the task"); it must be closed from its window or Task Manager.

3. **Session leak on partial failure.** `VR_InitSession` returned on swapchain failure
   *without destroying the session it had already created*, and `VR_BeginFrame` retried every
   frame — reaching `XR_ERROR_LIMIT_REACHED` within seconds. Added `VR_DestroySession()`
   (safe on a partially built session) and a 3-attempt cap.

Defences added: a named-mutex single-instance guard (a second copy runs flatscreen with a
clear log message rather than fighting for the runtime — which also makes local two-instance
multiplayer testing behave), and a run.bat pre-flight that kills any leftover instance and
warns loudly if one refuses to die.

**Rule of thumb: black headset + working audio/input == check for stray processes first.**

## FINDING 016 — HUD and hands verified working; both were misread as broken

- **"No HUD until the HEV suit" is correct Half-Life behaviour**, not a bug. HL draws no HUD
  without the suit. Its appearing on suit pickup actually *proves* the per-eye HUD
  compositing works.
- **"No hands" was no weapon.** Telemetry shows `hands=Lok/Rok` — both controller poses track
  fine. HL's viewmodel *is* the weapon-plus-hands, so with no weapon equipped there is simply
  nothing to draw. `+impulse 101` was not taking effect because it runs before the player
  spawns. run.bat now also binds **F9** = `impulse 101`, **F10** = `give item_suit`,
  **F11** = both.

---

## FINDING 017 — **HEADLINE** — Fire from the controller with an *unmodified* game DLL

The single most important architectural result in this fork so far.

### The problem

Shots came out of the player's face. Every stock Half-Life weapon traces from
`CBasePlayer::GetGunPosition()` (`glock.cpp:156`, `crowbar.cpp:153`, `gauss.cpp:324`, ...),
which in vanilla returns:

```cpp
Vector CBasePlayer::GetGunPosition() { return pev->origin + pev->view_ofs; }   // the EYE
```

Two separate defects hide behind "I shoot from my face":

| | source | fixable engine-side? |
|---|---|---|
| **Direction** | `pev->v_angle`, set from `usercmd.viewangles` | **Yes** — engine already sends it |
| **Origin** | `pev->origin + pev->view_ofs`, read inside the game DLL | looked like **no** |

Direction was fixed by writing weapon aim into the outgoing usercmd. Origin looked impossible
without touching `hl.dll`.

### How the reference ports solved it — and why we can't copy them

Both fork the SDK and override the virtual:

- **HLVR** — `src/dlls/player.cpp:4763` returns the weapon model's **muzzle attachment point**.
- **Lambda1VR** — `hlsdk-xash3d/dlls/player.cpp:371` returns `GetWeaponPosition()`, the tracked
  controller position.

Correct for them, useless here. **A forked `hl.dll` only fixes the one mod it was built for.**
The goal of this fork is playing *arbitrary* mods with their own custom game code, so any
solution requiring a per-mod rebuild is a non-solution.

### The actual fix

**`entvars_t` is ENGINE memory. The game DLL *reads* `view_ofs` — it does not own it.**

So don't move the shot; move the thing the shot is measured from. Point `view_ofs` at the
controller for exactly the one call that fires the weapon, then put it back:

```
engine/server/sv_pmove.c, SV_RunCmd():

    save   clent->v.view_ofs
    set    clent->v.view_ofs = muzzle - clent->v.origin      // VR_GetWeaponAim()
    call   svgame.dllFuncs.pfnPlayerPostThink( clent )       // -> ItemPostFrame -> weapon fires
    restore clent->v.view_ofs
```

`PostThink` is the right and only hook: `CBasePlayer::PostThink()` calls `ItemPostFrame()`,
which is where weapons actually fire. It must also be applied *after* `SV_FinishPMove()`, since
that copies pmove's duck-adjusted `view_ofs` back onto the entity and would otherwise clobber it.

### Why this satisfies every constraint at once

- **Custom mods** — stock `hl.dll` is untouched. Any mod, any custom content, no per-mod work.
- **Co-op with desktop players** — nothing is added to the network protocol, so vanilla clients
  connect normally.
- **Other players unaffected** — gated on `NET_IsLocalAddress( cl->netchan.remote_address )`.
  On a listen server `SV_RunCmd` runs for *every* client; remote desktop players must be
  left alone.
- **Nothing else observes it** — restoring immediately means networking, prediction and the
  client's own view never see the substituted value. This matters: an earlier attempt at the
  direction fix wrote weapon yaw into `cl.viewangles`, corrupting the injected-yaw bookkeeping
  in `VR_OverrideViewAngles` and compounding head yaw into an **uncontrollable spin**.
- **Dual wield later** — same mechanism, second position.

### Limitation

Works when the VR player **hosts** (singleplayer, or hosting the co-op server). If the VR player
*joins* someone else's server, origin reverts to the eye — that server cannot see the controller
without a protocol extension. Accepted deliberately: protocol changes would break crossplay with
vanilla clients, which is a stated priority.

### Generalisable lesson

When a game-DLL value blocks something, check whether the field lives in `entvars_t` first.
Anything the engine owns can be substituted around a specific game-DLL call without forking the
mod. This is likely reusable for other "the game DLL decides this" problems.

---

## OPEN QUESTIONS

1. ~~Does the runtime support `XR_KHR_opengl_enable`?~~ **ANSWERED — YES, see FINDING 007.**

2. 32-bit vs 64-bit final decision — SteamVR is 64-bit-only (FINDING 003), but VDXR supplies
   32-bit (FINDING 004) and is confirmed working (FINDING 007). **Leaning: stay 32-bit**, keep
   stock mod-DLL compatibility, target VDXR. Cost: SteamVR-only headsets unsupported in 32-bit.
   Note: Lambda1VR carries source-buildable mod forks as submodules
   (`hlsdk-xash3d`, `hlsdk-xash3d-opfor`, `hlsdk-xash3d_theyhunger`, `hlsdk-xash3d_aomdc`),
   which would matter if we ever go 64-bit. **They Hunger source port exists there.**

3. Do TFC (appid 20) / Ricochet (appid 60) appear owned? Not seen in `libraryfolders.vdf` —
   only Half-Life (70) and HL:VR (1908720). May need acquiring, or may be installed elsewhere.

---

## Feature priority (user-defined)

1. OpenXR bridge — stereo render + head/controller tracking  ← **CURRENT**
2. Room-scale origin reconciliation (port from HLVR `player.cpp`: `vr_ClientOriginOffset`, `vr_lastHMDOffset`)
3. Controller-driven aim + basic fire (HLVR attachment-based `GetGunPosition` / `GetAutoaimVector`)
4. Controller / viewmodel rendering
5. Physics grab/throw/melee (HLVR `VRControllerInteractionManager` + ReactPhysics3D)
6. Locomotion — ladders, teleport, ledge grab
7. VR-native UI (Nuklear laser-pointer menu)
8. Wishlist — dual wield, weapon hand-swapping, physical reload, articulated weapons,
   throwable grenades **with arc indicator**
9. Networking — **explicitly low priority**, neither reference project solved it

## FINDING 018 — Co-op and netplay: what is reachable, and what the protocol forbids

Researched against the engine source rather than assumed. Every claim below has a
citation; the one thing that is *not* verified is called out at the end.

**The engine already has real multiplayer.** `svs.maxclients` comes from
`sv_maxclients` (`engine/server/sv_main.c:138`), networking turns on whenever
`maxclients > 1` (`engine/server/sv_init.c:838`), and dedicated servers are a
separate build target (`engine/wscript:271`). A `coop` cvar exists engine-side
(`sv_main.c:76`) and is handed to the game DLL as `svgame.globals->coop`
(`sv_init.c:1016`).

**GoldSrc compatibility is one-way.** As a *client* this engine can join real
GoldSrc servers on protocol 48 (`engine/client/cl_main.c:1333`). As a *server* it
speaks only Xash protocol 49 and rejects anything else outright
(`engine/server/sv_client.c:338-341`). So a stock GoldSrc client can never join
us; anyone joining must run this fork.

**What VR loses when you join instead of host.** Every server-side substitution is
gated on `NET_IsLocalAddress` (`sv_pmove.c:977`, `:1074`) and wrapped in
`#if !XASH_DEDICATED`, so a dedicated build contains no VR code at all. Aim
*direction* survives, because it is written into the outgoing `cmd->viewangles`
(`cl_main.c:860`), as do buttons and movement. What is lost is anything needing
the server to act on controller data: shots leave the eye instead of the muzzle,
touch-to-use stops working, and dual wield goes inert.

**~~Sending controller pose to a remote server is a protocol break.~~ — WRONG,
CORRECTED BY FINDING 020.** This paragraph originally read: *"`usercmd_t` has
`int32_t reserved[4]` but all four are already spent on FWGS's
`impact_index`/`impact_position`. Adding pose fields changes
`STATIC_CHECK_SIZEOF(usercmd_t, 52, 52)`, the delta table and the read/write
pair — and mismatched clients are then rejected at `sv_client.c:338`."*

The four fields are **named, not spent**. The binding at
`engine/common/net_encode.c:69-72` is the entire set of references to
`usercmd_t.reserved[]` in the whole tree — nothing reads them, nothing writes
them, nothing validates them. Crossplay needed no protocol break at all, and
this claim steered the roadmap wrong for as long as it stood. See FINDING 020.

**There is no maintained co-op mod we can run.** Sven Co-op is out for a stated
reason — "Uses custom GoldSrc engine"
(`Documentation/not-supported-mod-list-and-reasons-why.md:19`). Searching the
whole supported list for co-op content turns up three entries and nothing
maintained (`Documentation/supported-mod-list.md:219, 675, 1037`).

**But `coop 1` may already work with a stock `hl.dll`, and this is the useful
finding.** The engine forces `deathmatch 0` when `coop` is set
(`sv_init.c:824, 1002`). `InstallGameRules` selects purely on
`gpGlobals->deathmatch` (`hlsdk-portable dlls/gamerules.cpp:320-344`), so
`coop 1` yields `CHalfLifeRules` — the *singleplayer* rules — with N clients.
Singleplayer rules always allow monsters
(`dlls/singleplay_gamerules.cpp:408-411`), unlike multiplayer rules which gate on
`mp_allowmonsters`, default off (`dlls/multiplay_gamerules.cpp:1103-1106`).
`trigger_changelevel` only blocks on `IsDeathmatch()`
(`dlls/triggers.cpp:1450-1452`), and `respawn()` respawns in place under coop
(`dlls/client.cpp:136`). Scripted sequences have no multiplayer gate at all.

So SP maps with several players may already run without touching any SDK — which
is the mod-agnostic outcome, since it needs no rebuilt game DLL.

**Two engine limits apply regardless.** Savegames force single player
(`engine/server/sv_save.c:2173-2175` sets `maxplayers 1`, `deathmatch 0`,
`coop 0`), and level transitions lose entity carry-over because
`svs.maxclients > 1` forces `smooth = false` (`engine/server/sv_game.c:747-748`).
A co-op campaign therefore has no saves and no inventory carried between maps.

**Ranked path.** (a) VR player hosts, others join — works today, zero code, but
joiners need this fork. (b) `coop 1; maxplayers N` for SP content — try before
writing anything, per above. (c) VR while joining someone else's server — needs
the protocol break, and is the only option that stops vanilla clients connecting.

**Not verified:** whether `coop 1` actually produces playable SP maps at runtime.
This is static analysis only.

## FINDING 019 — Where the project actually stands against the Half-Life campaign

An honest assessment after an adversarial audit of the whole VR layer (42 agents,
every finding put to independent skeptics before being believed).

**Roughly 80% for playing the Half-Life campaign start to finish.**

What is done and confirmed working in a headset: stereo rendering, head and hand
tracking, hands and weapon models, firing from the controller with an unmodified
game DLL, muzzle-attachment-derived aim, laser sight, grenade arc, two-handed
stabilisation, melee by swing, flashlight, haptics, stick locomotion with snap
and smooth turn, HUD in the headset, room-scale walking, physical crouch, ladder
climbing, touch-to-interact, stair smoothing, and the menu rendering in VR.

What is missing or unproven, in the order it would block a playthrough:

1. **Nothing has been played end to end.** Every feature has been tested in
   isolation, on one map, usually for a minute. The campaign has set pieces —
   trains, tank turrets, the Xen jumps, the tentacle sequence — that nobody has
   attempted in VR at all.
2. **Weapon selection is cycle-only.** No direct slot access and no working menu
   navigation with the controllers.
3. **No teleport locomotion.** Smooth-only is a comfort problem for some players
   and there is no alternative.
4. **Train and tank controls are untested**, and are the one campaign-critical
   interaction the reference ports both needed game-DLL work for.
5. **Long jump** is likely unusable, and Xen requires it.

The 20% is therefore mostly *verification*, not construction. The features exist;
what does not exist is evidence that a full campaign survives them. The audit
above is a fair warning on that point: it found thirteen real defects, most of
them shipped the same day, several of which only appear after a level change or a
weapon swap — exactly the conditions a single-map test never reaches.

---

## FINDING 020 — Audit against both reference ports, and what it turned up

The fork was inventoried feature-by-feature against Lambda1VR and HLVR, then
every claimed gap was put back to the source and an attempt made to *refute* it
rather than confirm it. Several claims died that way, which is the point. What
survived is below, with what was done about it.

### The comparison, in one line each

**Lambda1VR** — Xash3D-FWGS like us, but Quest-only and per-mod: four forked
HLSDK submodules. Genuinely good ideas we lacked: physical scope engage,
throw-velocity-from-hand-motion, reach-behind gestures, usable-object
highlighting.

**HLVR** — real GoldSrc, OpenVR, one forked SDK, one mod. Much deeper
interaction (ReactPhysics3D grab/drag, valves, ledges, trains, NPC touch
commands) and a 119-setting config app. Two things worth knowing: it has **no**
two-handed grip (still an open issue upstream), and its **haptics never fire** —
every `TriggerHapticVibrationAction` call is commented out. We are ahead on
both.

Neither has: mod-agnostic operation, model-metadata weapon classification,
multi-view stereo (mod monitors/mirrors keep working), a ballistic grenade arc,
or a 64-bit build.

### Defects found, all now fixed

1. **The play space did not follow the body.** `VR_SetWorldReference` advanced
   the sync point by the body's full delta while `VR_PlayToWorld` rotated it
   back by the exact inverse, so the terms cancelled identically and the eye and
   both hands were *mathematically invariant* to body motion. Not a train bug —
   stick locomotion, conveyors, `basevelocity` and NPC pushes were all silently
   subtracted out. `vr_diag.log` had been recording the evidence the whole time:
   `lean` climbing 174 → 740 units over twenty seconds. Now only the share of
   motion room-scale actually commanded closes the loop.

2. **Nothing stopped a muzzle inside geometry.** The exploit half (shooting
   through cover) mattered less than the involuntary half: with the muzzle in
   solid, the mod's own trace returns `startsolid` and the shot dies silently.
   Bracing a rifle on cover made bullets stop existing. Origin is now clamped to
   the last clear point.

3. **Trains could not be driven.** `PlayerUse` reads
   `m_afButtonPressed & IN_FORWARD|IN_BACK` and never looks at `forwardmove`.
   The VR path wrote only axes, so mounting the tram controls worked and
   accelerating did not — "On A Rail" was impassable.

4. **Stock auto-aim was on and fighting us**, deflecting `pev->v_angle` — which
   in VR *is* the tracked weapon direction — by up to 25°, and desyncing the
   bullet from the laser. `sv_aim`/`sv_allow_autoaim` are engine-owned cvars the
   game DLL merely reads, so bracketing them needs no DLL cooperation. This is
   the cleanest instance of the fork's core rule we have found.

5. **Mounted guns aimed 45° high.** `CFuncTank::StartControl` nulls the
   viewmodel, so the no-attachment fallback applied a mesh correction with no
   mesh present. Tank *aiming* otherwise already worked for free — `func_tank`
   mirrors `pev->v_angle`, which we already write.

6. **Audio was spatialised from the mod's flat camera**, which does not turn
   with the head, move with room-scale, or drop on a crouch.

7. **Weapon classification was inverted on real content.** `melee = !fires` with
   `fires` keyed partly on `numattachments > 0` — which carries no melee
   information on Valve-derived models. A four-attachment VR crowbar read as a
   gun, so **swing-to-hit was silently off on the crowbar**; retail `v_satchel`
   and `v_tripmine` read as melee, so waving an arm planted tripmines. Melee is
   now positively identified and defaults off, because the failure modes are not
   symmetric.

8. **The muzzle path never ran on retail content.** `VR_ModelHasBore` needs two
   attachments; every retail HL/Op4/BS gun ships exactly one. It only ever
   appeared to work because VR-authored models carrying four were installed over
   the top — see the warning below. A single attachment now supplies the origin.

### Also added

Teleport locomotion (`vr_teleport`, off by default, built as a mode on the
existing stick so no interaction profile changed); HUD stereo depth
(`vr_hud_parallax` — the HUD had been sitting at infinity and doubling);
eye-to-hand reachability gating for touch-use; and **VR-to-VR crossplay**
(`NET_EXT_VRPOSE`), which corrected FINDING 018 above.

### The warning that matters most

`E:\XashVR\valve\models\` contains **byte-identical copies of HLVR's own
VR-authored model set** — `v_crowbar.mdl` md5-matches theirs exactly. They sit
ahead of `-rodir` in the search path, so they load in preference to retail
content *and leak into every mod that falls back to `valve`*.

Every in-headset result this project has ever produced was measured on another
project's prepared assets. That is what hid defects 7 and 8 for so long, and it
means mod-compatibility claims are unproven in a way FINDING 019's "80%" does
not capture — that figure was scoped to Half-Life, and Half-Life was being
played with someone else's models.

**Nothing should be concluded about mod compatibility until those models are
removed and the retail set is what actually loads.** The classifier fix was
verified by parsing both sets and confirming identical verdicts, which is the
right bar for the rest of it too.
