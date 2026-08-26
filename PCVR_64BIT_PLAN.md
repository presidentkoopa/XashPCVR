# Plan: a 64-bit build alongside the 32-bit one

## Why

Modern standalone Xash titles ship **amd64-only** game binaries. Diffusion
(`bin/server_amd64.dll`, `client_amd64.dll`, `menu_amd64.dll`, all confirmed
x86-64) cannot be loaded by a 32-bit engine at any price — wrong architecture,
and `LoadLibraryW` simply fails.

This is **additive, not a migration.** 32-bit stays the default and stays the
build that plays the legacy catalogue, because `COM_GenerateServerLibraryPath()`
uses a mod's declared DLL filename verbatim only on `XASH_X86 && XASH_WIN32`
(`engine/common/lib_common.c`). Every other architecture rewrites it with an
`_amd64` suffix, which no legacy mod ships. Losing that would cost ~1200 mods to
gain a handful.

So: two binaries, one VR layer.

## What is already in place

- **SDL2** — `E:\deps\SDL2-2.32.10` carries both `lib/x86` and `lib/x64`. Free.
- **waf** — a `-8` / `--64bits` configure flag already exists (`wscript`), which
  sets `ALLOW64` and skips `force_32bit()`.
- **The VR layer is architecture-agnostic C.** It holds no pointer-width
  assumptions by design, so no port is expected — only compilation.

## The known blocker

**The OpenXR loader is 32-bit only.** `E:\OpenXR-SDK` contains a `build32`
directory and nothing else, so there is no 64-bit `openxr_loader.lib` to link
against. This has to be built before anything else can proceed.

## Steps

1. **Build a 64-bit OpenXR loader.** CMake the SDK at `E:\OpenXR-SDK` into a
   separate `build64` so the existing 32-bit loader is left intact.
2. **Confirm the runtime serves 64-bit apps.** VirtualDesktopXR is the runtime
   here (PCVR_LOG FINDING 007). A 64-bit app needs a 64-bit runtime DLL
   registered; verify before assuming.
3. **Configure a second waf build.** `./waf.bat configure -8` with the x64 SDL2
   and the new loader, into a **separate output directory** so the 32-bit build
   is not clobbered — waf keeps its configuration in `build/c4che`, so the two
   cannot share one output tree.
4. **Fix whatever 64-bit compilation surfaces.** Expect pointer-to-int casts and
   format-specifier warnings rather than anything structural.
5. **Decide the deployment layout.** Determine whether a 64-bit engine looks for
   `ref_gl_amd64.dll` — if it does, both sets of modules can live in one folder
   and the launcher just picks the exe; if not, they need separate directories.
6. **Test against Diffusion**, which is the only reason this exists.
7. **Regression-test the 32-bit build.** Nothing here should touch it, and that
   assumption is worth checking rather than trusting.

## Risks

- **The OpenXR runtime may not serve 64-bit.** That would stop the whole plan,
  and it is the first thing to establish.
- **Config and save directories may collide** between the two builds. Worth
  settling deliberately rather than discovering.
- **Two binaries means two of every VR bug.** Every issue gets chased twice. The
  mitigation is to keep the VR layer arch-clean so the two never diverge in
  behaviour.

## Explicitly out of scope

- **vgui2.** Ruled out. Counter-Strike 1.6, Condition Zero and Day of Defeat stay
  unsupported. Where those matter, the route is a recreated client DLL, never
  implementing vgui2 in the engine.
- **Migrating to 64-bit.** 32-bit remains the default.
