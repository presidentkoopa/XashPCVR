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
