# OpenComposite — working notes

OpenComposite implements SteamVR's OpenVR API and forwards to OpenXR, so SteamVR games run
without SteamVR. The DLL is built as `vrclient_x64.dll` and installed by copying it over a
game's `openvr_api.dll`.

This file records the state of this checkout as of **2026-08-13**. Everything below was
established by building and running against real games, not by reading alone.

---

## Build

```bash
cmake -S . -B build -A x64
cmake --build build --config Release --target OCOVR
# -> build/bin/Release/vrclient_x64.dll
```

Use `--target OCOVR`, not `ALL_BUILD`: the latter also builds `RuntimeSwitcher` (C#), which
needs the .NET Framework 4.8 dev pack and is unrelated to the DLL.

### Dependencies are hand-installed

This directory is **not a git repo** (no `.git`), so `git submodule update --init` cannot work.
The dependencies were fetched manually:

| Path | Source | Version | Notes |
|---|---|---|---|
| `libs/glm` | g-truc/glm | `1.0.1` | header-only |
| `libs/openxr-sdk` | KhronosGroup/OpenXR-SDK | `release-1.1.62` | **needs `--recurse-submodules`** for `src/external/jsoncpp` |
| `libs/vulkan/Include` | KhronosGroup/Vulkan-Headers | `v1.3.280` | not a submodule; CMake just expects it |
| `libs/vulkan/Lib{,32}/vulkan-1.lib` | generated locally | — | see warning below |

**Do not downgrade the OpenXR SDK.** An earlier attempt with `release-1.0.34` compiled and linked
fine but produced a DLL that hung at `xrGetSystem` or `D3D11CreateDevice` non-deterministically
(sometimes crashing with `STATUS_HEAP_CORRUPTION`) against VirtualDesktopXR. Upgrading the loader
fixed it. This cost several hours — don't repeat it.

`libs/libovr` and `libs/libunwind` are empty and that's fine: `libovr` is unreferenced by CMake,
`libunwind` is only used with `OC_BACKTRACE=ON` (default off).

### The Vulkan import libraries are a local stopgap

`vulkan-1.lib` was synthesised from the system `vulkan-1.dll` exports via `dumpbin /EXPORTS` +
`lib /def:`. The x64 one covers all 265 exports. **The x86 one only aliases the ~19 Vulkan
functions the code currently calls** — x86 Vulkan is `__stdcall`, so the `.def` needs decorated
`vkFoo@N = vkFoo` entries with *no* leading underscore (lib.exe adds it). Calling a new Vulkan
function breaks the 32-bit link until the `.def` is regenerated. The proper fix is installing the
LunarG Vulkan SDK and using its real import library.

### Toolchain

VS 2022 **Build Tools** + MSVC 14.44. **ATL is not installed and is not needed** — see the
upstream include bugs below. `clang-format` is unavailable, so `format-all.sh` can't verify style.

---

## Gotchas that cost real time

**The codegen does not re-run when you edit `CVR*.cpp`.** Its CMake dependencies don't track those
files, so adding a `GEN_INTERFACE` silently produces a DLL without it. After touching interface
registrations:

```bash
rm -rf build/generated && cmake --build build --config Release --target OCOVR
```

Verify with `grep -ac IVRSomething_0NN build/bin/Release/vrclient_x64.dll` — it should be non-zero.

**Adding an OpenVR interface version takes four edits, not one:**
1. drop the OpenVR header in `OpenVRHeaders/`
2. add the version to the hardcoded `versions` list in `scripts/split_headers.py` (the CMake glob
   is only for dependency tracking)
3. add `GEN_INTERFACE("Name", "0NN")` in the matching `OpenOVR/Reimpl/CVRName.cpp`
4. implement any new methods on the `Base*` class — the build fails with
   `'Foo': is not a member of 'BaseBar'` and names them

**Never leave `logAllOpenVRCalls=true` in `opencomposite.ini` for normal play.** One race produced
a **970 MB, 8-million-line** log. It's invaluable for diagnosis — the last line names the function
that crashed — but it writes a flushed line per OpenVR call.

**The log keeps only one previous run**, as `opencomposite.log.1` (since 2026-08-14 — it used to
truncate outright). Two launches still lose the run you cared about, so copy it aside first.

---

## Changes made to this checkout

### Upstream portability bugs (would recur after any upstream merge)

- `OpenOVR/Compositor/compositor.h`, `dx12compositor.cpp` — removed `#include <atlbase.h>`. Both
  only use WRL's `ComPtr`. ATL is a separate optional VS component absent from Build Tools. The
  code that genuinely uses ATL (`dx10compositor`, `VRKeyboard`) is fully `#ifdef`'d out here.
  **If a fresh pull fails with `Cannot open include file: 'atlbase.h'`, delete the include — don't
  install ATL.**
- `DrvOpenXR/XrHMD.cpp` — added `<chrono>` (used `chrono_literals` via transitive include)
- `DrvOpenXR/DrvOpenXR.cpp` — added `<shlwapi.h>` + `#pragma comment(lib, "shlwapi.lib")` for
  `PathStripPathA`
- `CMakeLists.txt` — conditionally adds the three loader sources OpenXR-SDK 1.1.x has and 1.0.x
  doesn't (`loader_init_data.cpp`, `loader_properties.cpp`, `xr_generated_dispatch_table_core.c`)

### F1 25 fixes (both real upstream bugs — worth submitting)

- **`IVROverlay_028`** — the game aborted at startup with an "IVROverlay_028" dialog. Added
  `OpenVRHeaders/openvr-2.12.1.h` (earliest release with 028; adds only `IVROverlay_028` and
  `IVRSystem_023` over the bundled 2.5.1), registered both, and implemented:
  - `BaseSystem::PollNextEventWithPoseAndOverlays` — forwards to `PollNextEventWithPose`, reports
    `k_ulOverlayHandleInvalid`. Should be behaviourally correct.
  - `BaseOverlay::CreateSubviewOverlay` / `SetSubviewPosition` — **stubbed**, return errors. No
    OpenXR equivalent. If F1 25's UI ever depends on these, expect missing elements, not a crash.
- **`XrBackend::GetFrameTiming` memset crash** — F1 25 sometimes passes an uninitialised
  `m_nSize` of **2070714990 (~1.9 GB)**. Upstream memsets `m_nSize - 4` bytes of the caller's
  buffer unbounded → access violation in `vcruntime140.dll`. Now bounds-checked to `4..512`.
  **The bound must stay generous.** F1 25's *legitimate* calls pass **192**, larger than
  `sizeof(OOVR_Compositor_FrameTiming)` (176), because newer `vr::Compositor_FrameTiming` appends
  two VSync counters. Clamping to our own struct size would reject valid calls.

### Shutdown abort — the "OpenComposite Error - info in log" dialog (2026-08-14)

`DrvOpenXR::ShutdownSession()` ended with an unconditional `CreateSystemID()`. That exists so the
*restart* paths (graphics-API switch, input rebind) have a fresh `XrSystemId` for the next
`xrCreateSession` — but `FullShutdown()` used the same function on the way out, where it's
pointless. If the headset had already gone away, `xrGetSystem` returned
`XR_ERROR_FORM_FACTOR_UNAVAILABLE` and `OOVR_FAILED_XR_ABORT` killed the game with a dialog.

**A headset that's disconnected is the normal reason to be shutting down, not an error.** Fixed by:
- `ShutdownSession(bool forRestart = true)`; `FullShutdown` passes `false` and skips the call
- `xrRequestExitSession` / `xrDestroySession` / `xrDestroyInstance` now log-and-continue on failure
  instead of aborting — we're tearing down regardless
- `CreateSystemID` retries `XR_ERROR_FORM_FACTOR_UNAVAILABLE` for 2s (20 × 100ms). With a streaming
  runtime that error is routinely transient; it also covers launching the game before the headset
  has finished connecting.

### Frame hitch watchdog (`hitchWarningMs`, default 30)

Frames slower than the threshold get one line naming the phase that ate the time:

```
HITCH frame 412: 218.4ms total | xrWaitFrame 205.1 | xrBeginFrame 0.1 | locateViews 0.2 | compositor 1.8 | xrEndFrame 0.3 | game 10.9 (hitch #7)
```

A healthy 72Hz frame is 13.9ms and logs nothing, so unlike `logAllOpenVRCalls` this is safe to leave
on. Output thins to every 100th hitch after the first 200 so a bad run can't produce another 970MB
log. **Read `xrWaitFrame` as "the runtime/streaming link is pacing us", `game` as "F1 25's own
render thread stalled", `compositor` as "our copy/swapchain".**

Timings reset when the session goes inactive — otherwise a headset-off gap gets measured as one
enormous frame.

### The log no longer truncates away the previous run

`logging.cpp` renames the old log to `opencomposite.log.1` before opening. Previously the log for a
failed run was destroyed the moment anything VR-related launched again, including a second attempt
at the same game — which is exactly when you need the run before last.

### Logger data race (real bug, found while debugging)

`OpenOVR/logging.cpp` — `std::ofstream` was written from multiple threads with **no
synchronisation**. That's UB, and the streambuf owns a heap buffer, so racing writers corrupt the
heap. Added a `std::recursive_mutex` (recursive because the abort macros log). Upstream flagged
this in a comment and never fixed it.

### Code review fixes

A full review was done first; these came out of it. See the sections below for which are verified.

**Memory/crash:** double `Release()` on `ComPtr<ID3D12Device>` (two sites: `dx12compositor.cpp`,
`XrBackend.cpp`); uninitialised `currentFenceValue`; `GetSkeletalReferenceTransforms` buffer
overflow; null-deref in `GetSkeletalBoneData` when hand-tracking has no interaction profile;
`GetHandSpace` indexing `legacyControllers[HAND_NONE]` out of bounds; DX11 MSAA sample-count
check restored so `resolvedMSAATextures` can't be indexed empty.

**Correctness:** `flags & (BIT != 0)` precedence bug in `xrmoreutils.cpp` (×2) — was testing
`ORIENTATION_VALID` instead of `POSITION_VALID`, which is why the hand-tracking fallback never
engaged when a controller was set down; DX12 bounds now use `CopyTextureRegion` (the computed
box was previously discarded and `CopyResource` used); DX11 MSAA resolve reads slice 0;
per-frame `RSGetState` reference leak; `BindInfoSet` end-iterator deref; unknown interaction
profile now falls back to `khr/simple_controller` instead of aborting; IPD fallback `0.0064`
→ `0.064`; Vulkan swapchain sample count vs resolve; 32-bit LUID `sizeof(&x)` → `sizeof(x)`.

**Performance:** atomic tracker count so `GetDevice` rejects the ~61 empty device slots
`WaitGetPoses` asks for each frame without locking; cached DX11 shader resource view instead of
recreating per frame; single map lookups on the per-frame input path.

**Compatibility:** DX11 shader-invert path now saves/restores render targets, blend state,
shaders, sampler and shader resources. Previously it clobbered them — a likely source of
game-specific rendering corruption.

---

## RESOLVED: the 2026-08-14 frame-time collapse — the headset link had dropped to USB2

**Cause was outside OpenComposite.** The headset was negotiating USB2 instead of USB3, so the
streaming link couldn't carry the video and everything downstream stalled. Once that was fixed the
symptoms vanished completely, at a *higher* resolution than the bad runs (2496x2688 vs 2248x2432,
+23% pixels):

| Run | avg FPS | worst frame | stutters |
|---|---|---|---|
| 08-14 06:00 (USB2) | 53 | **326.2ms** | 61 |
| 08-14 07:13 (fixed) | 69 | **17.8ms** | **0** |

The hitch watchdog settled it in one run: **96% of hitch time was in `game`, 0.0% in `compositor`,
1.4% in `xrWaitFrame`.** Our copy path never exceeded 0.1ms.

**Don't chase the remaining HITCH lines.** With `hitchWarningMs=30` the watchdog still fires in
menus and on loading screens, because F1 25 renders its menus at a 30fps cap — that's the 33.2ms
median hitch, and it's the game behaving normally, not a fault. The multi-second outliers (2.9s,
3.8s) are track loads where the game simply stops calling `WaitGetPoses`. During actual driving the
benchmark's worst frame was 17.8ms, i.e. the watchdog is silent. Raise the threshold to ~50 if the
menu lines get annoying.

Caveat on attribution: the `game` bucket is wall time between our `SubmitFrames` returning and the
next `WaitGetPoses`, so it includes the game's own GPU work. A starved GPU would also land there.
What it does prove conclusively is that our compositor and the OpenXR calls are not the bottleneck.

### Diagnostic method that worked

The game's own benchmark XMLs (`Documents\My Games\F1 25\benchmark\*.xml`) were far better evidence
than the OC log for the perf question — they record frame-time percentiles, a stutter count, and a
copy of every graphics setting, so two runs can be diffed directly. Check them first for any F1 25
performance complaint. Three things they let us rule out before touching any code:

- **Not a code regression.** The DLL running during the bad runs differed from
  `openvr_api-KNOWN-GOOD.dll` by 152 bytes — PE timestamp plus `__LINE__` constants shifted by
  comment edits, identical string tables. Byte-diff the DLLs; don't trust mtimes.
- **Not GPU load.** Settings had been *lowered* between the good and bad runs and it got worse.
- **Not swapchain thrash.** Only the 4 expected `Generating new swap chain` lines at startup.

## Current state

**Working:** F1 25 and Automobilista 2, both via VirtualDesktopXR, no SteamVR. F1 25 holds a
steady 72 FPS (13.89 ms, tight maxima) — that's the Quest 3 refresh cap, not a GPU limit.

**F1 25 is DX12.** AMS2 is DX11. Relevant because the DX11 and DX12 paths differ a lot, and the
DX12 path has no MSAA support at all.

### Verified by running

The DX11 copy path, DX12 `CopyResource` path, session lifecycle, interface registration, the
`GetFrameTiming` bound, and the logger mutex.

### NOT verified — check these first if a game misbehaves

- **DX11 state save/restore** — needs `invertUsingShaders=true`; neither game exercised it
- **DX12 bounds copy** (`CopyTextureRegion`) — only runs when bounds are non-null, which needs
  `invertUsingShaders=true`
- **Hand-tracking fallback** — needs controllers set down mid-session
- **MSAA resolve** (DX11 and Vulkan) — neither game submitted multisampled textures
- **Anything Vulkan** — no Vulkan title tested
- **32-bit build** — compiles and links, never run

### Performance

Measured, don't guess. A measurement pass found: F1 25 calls `GetFrameTiming` **once**, not per
frame, so the "feed real frame timings to fix dynamic resolution" idea is **dead** — it would
achieve nothing. The frame interval is pinned to the 72 Hz display cap. Remaining code-level
wins (per-frame pose cache, MSAA resolve-direct-to-swapchain) are well under a millisecond on a
frame that already finishes early.

**The real lever is Virtual Desktop's refresh rate (72 → 90 Hz), not code.** Second lever is
`supersampleRatio` in the ini. OpenComposite has no FSR/upscaling and shouldn't — use OpenXR
Toolkit, which layers *after* OpenComposite in the OpenXR chain. Note `openvr_fsr` is
incompatible: it also replaces `openvr_api.dll`.

---

## Deployment

```bash
cp build/bin/Release/vrclient_x64.dll "<game>/openvr_api.dll"     # note the rename
```

- F1 25: `F:\Programs\Steam\steamapps\common\F1 25\` (backup: `openvr_api.dll.backup`, Valve's original)
- AMS2: `F:\Programs\Steam\steamapps\common\Automobilista 2\x64\` (`openvr_api.dll.backup` is stock
  OpenComposite, `openvr_api-orig.dll` is Valve's)
- Known-good build kept at `build/openvr_api-KNOWN-GOOD.dll`

The OpenXR runtime must be **VDXR**, set in Virtual Desktop Streamer settings. Selecting SteamVR
as the OpenXR runtime defeats the entire point (OpenComposite would hand off to SteamVR) — check
the `Runtime:` line in the startup log if behaviour is odd.

Log: `%LOCALAPPDATA%\OpenComposite\logs\opencomposite.log`

A one-time `[startup]` trace is compiled in — ~25 lines at init, no cost during play. It
localised every failure hit during this work; keep it until things are proven stable.

**F1 25 runs EA Javelin Anticheat.** No evidence it interfered, but it hasn't been ruled out, and
replacing DLLs in an anticheat-protected game carries some risk. Keep the backups.

---

## Working style notes

Debugging this went badly when driven by inference and well when driven by instrumentation.
Several confident diagnoses were wrong (blaming the anticheat, calling a hang a crash, inventing
a mechanism for a setting the user had changed themselves). What actually worked, every time:
add a log line, run once, read the answer.

The `[startup]` trace and `logAllOpenVRCalls=true` turn "it crashes" into a named function within
a single run. Reach for them early rather than reasoning from symptoms.
