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
Runs **flatscreen** — `VR_Init()` still has zero call sites.

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
