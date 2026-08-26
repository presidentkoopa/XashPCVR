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

## The known blocker — RESOLVED

**The OpenXR loader was 32-bit only**, and it was worse than a missing library:
it was a hard build failure rather than a graceful fall back to flatscreen.
`engine/wscript:113` picks `build32` or `build64` from `DEST_SIZEOF_VOID_P` and
sets `conf.env.OPENXR = False` when the loader is absent — but the source glob
at `engine/wscript:299` is unconditional, and `vr_openxr.c` was guarded only by
`#if XASH_WIN32 && !XASH_DEDICATED`. So the file compiled anyway and died on
`#include <openxr/openxr.h>` with no include path.

That guard is now `#if XASH_WIN32 && !XASH_DEDICATED && XASH_OPENXR`, and the
no-OpenXR stub block has been brought up to date — it covered 13 functions while
the header declared ~50, so that branch had quietly stopped linking long ago and
nobody noticed, because every build anyone made had OpenXR.

**A 64-bit loader now exists**, built from the SDK already on disk:

```
cmake -S E:/OpenXR-SDK -B E:/OpenXR-SDK/build64 -G "Visual Studio 18 2026" -A x64 -DDYNAMIC_LOADER=ON
cmake --build E:/OpenXR-SDK/build64 --config Release --target openxr_loader
```

It lands at exactly the path `engine/wscript:115` probes, `build32` is untouched,
and configure now reports the 64-bit loader as found. No build-system changes were
needed. (`cmake` is not on PATH; it ships with Visual Studio.)

**And the runtime serves 64-bit.** VirtualDesktopXR installs both runtimes side by
side: the native registry view has `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`
pointing at a json whose library is x86-64, while the WOW6432Node view points at
the 32-bit one. A 64-bit build resolves to the x64 runtime with no user action.

Still unconfirmed: the capability probe in FINDING 007 (extensions, view sizes, GL
version) was run by the **32-bit** probe against the **32-bit** runtime DLL. Almost
certainly identical, but rebuilding `vr/probe` as 64-bit would settle it.

## Steps

1. ~~Build a 64-bit OpenXR loader.~~ **Done** — see above.

2. ~~Confirm the runtime serves 64-bit apps.~~ **Done** — VirtualDesktopXR ships an x64 runtime and it is the registered ActiveRuntime.


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

- ~~The OpenXR runtime may not serve 64-bit.~~ Resolved: it does.

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
