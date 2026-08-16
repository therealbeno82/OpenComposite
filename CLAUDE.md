# OpenComposite — working notes

OpenComposite implements SteamVR's OpenVR API and forwards to OpenXR, so SteamVR games run
without SteamVR. The DLL is built as `vrclient_x64.dll` and installed by copying it over a
game's `openvr_api.dll`.

This file records the state of this checkout as of **2026-08-16**. Everything below was
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

This directory is a **local-only git repo** — one branch, no remote — so
`git submodule update --init` cannot work. (It genuinely had no `.git` before 2026-08-15; that
changed, but the dependency situation did not.) The dependencies were fetched manually:

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
This bit *has* since eaten a run that mattered: anything worth keeping goes to a
`opencomposite-<date>-<what>.log` in the same folder, out of the rotation's way.

**`logGetTrackedProperty=true` is cheap and underused.** Unlike `logAllOpenVRCalls` it logs only
property queries, and it answered "which property does this game actually key off?" in a single
run — the answer for F1 25 was *one* property for the whole session. Reach for it before
theorising about what a game reads from us.

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

### Second review pass (2026-08-16)

Compile-verified and run against F1 25. Grouped by what each would actually cost you.

**Memory corruption:** `GetCachedViews` inserted into an `unordered_map` while holding only a
`std::shared_lock` — two threads on a cache miss corrupt the map (this was a *regression* from an
earlier uncommitted "optimisation"; the insert now takes the exclusive lock, with the runtime call
outside it). Three `delete` on `new[]` allocations in `BaseRenderModels`. `handTrackers[2]` indexed
with `HAND_NONE` (== 3) from `GetSkeletalSummaryData`.

**Working-tree regression:** `UpdateActionState`'s `xrSyncActions` buffer was moved from a
per-frame `std::vector` (zeroed every frame) to a persistent member (zeroed once), but
`subactionPath` is only written conditionally — so a set restricted to one hand on one frame stayed
restricted forever. Slots are now cleared per frame.

**Exceptions escaping the C ABI:** four sites in `BaseInput` threw `std::out_of_range` out through
OpenVR into the game — `replace(find(...), ...)` with no `npos` check (×2), `int endOfHandPos`
truncating `npos` to −1 before `erase`, and `parts.at(3)/.at(4)` on paths with fewer components.
Bound-source paths come from the game's binding JSON and are not guaranteed to look like
`/user/hand/<side>/input/<x>/<y>`. Also `strcpy_arr` → a truncating variant at every site fed by
game data; `strcpy_s` invokes the MSVC invalid-parameter handler (process termination) on overflow,
and `escapePathString` can *grow* a name past `XR_MAX_ACTION_NAME_SIZE`.

**DX12:** resource barriers around every copy/resolve, and the swapchain image returned to
`RENDER_TARGET` before `xrReleaseSwapchainImage` as `XR_KHR_D3D12_enable` requires — there were
none at all. Array-texture slice selection (both eyes previously got slice 0, i.e. the left image
twice). Degenerate `D3D12_BOX` guard. Finite fence-wait timeout and a checked
`SetEventOnCompletion`, so a TDR can't hang the render thread forever. Removed a `_DEBUG`
`D3D12GetDebugInterface` null-deref that fires on any machine without the Windows "Graphics Tools"
feature — and which ran *after* device creation, so it was a no-op even when it worked.
**Deliberately not done:** transitioning the *source* texture. OpenVR does not specify what state a
game hands it over in, and naming the wrong `StateBefore` is worse than naming none.

**Frame-timing API:** `GetFrameTimings` passed `unFramesAgo = 1`, which the backend rejected, so it
always returned 0 and wrote nothing. And the `unFramesAgo > 0` guard itself was wrong — OpenVR
documents *"sets oldest timing info if nFramesAgo is larger than the stored history"*, i.e. clamp,
not fail. **F1 25 asks for 5 frames ago and was getting nothing.** `GetFrameTimeRemaining` and
`GetCumulativeStats` were `STUBBED()`, which is `OOVR_ABORT` — they now return sane values instead
of killing the game. `GetTimeSinceLastVsync` now zeroes `pulFrameCounter` per its contract.

**Also:** `SubmitWithArrayIndex` read `idx * sizeof(Texture_t)` past the caller's struct;
`handColour` alpha was `255` in a 0..1 float field (giving near-transparent hands); overlay
container locking between the render and game threads; `InteractionProfile`'s static tables built
via a proper thread-safe initialiser; hand trackers released before their session is destroyed;
`uint16_t` loop counter against a `uint32_t` vertex count; `split_face` substring length swapped.

**Checked and *not* a bug** (so it doesn't get "fixed" again): `logging.cpp`'s
`duration<long, std::milli>` does overflow 32-bit `long`, but the truncation cancels in the
subsequent subtraction and yields the correct millisecond value. The log's own timestamps confirm it.

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

---

## RESOLVED: the "half FPS" mystery — Virtual Desktop's Synchronous SpaceWarp (2026-08-16)

**Also outside OpenComposite.** F1 25's counter was reporting exactly half the headset refresh, and
it was telling the truth — the game really was rendering at half rate while SSW synthesised the
intermediate frames, which is why motion still looked smooth.

The benchmark XMLs settled it without touching code. Four different refresh rates, four exact
halvings, reproduced across pairs of independent runs:

| Run | avg frame time | fps | headset Hz | ratio |
|---|---|---|---|---|
| 08-15 19:25 | 13.885 ms | 72 | 72 | full rate |
| 08-15 19:58, 20:06 | 22.227 ms | 45 | 90 | **exactly ½** |
| 08-15 22:47 | 27.785 ms | 36 | 72 | **exactly ½** |
| 08-15 22:53, 23:05 | 25.008 ms | 40 | 80 | **exactly ½** |
| 08-15 23:12, 23:17 | 16.667 ms | 60 | 120 | **exactly ½** |

1000/45 = 22.2222, 1000/36 = 27.7778, 1000/40 = 25.0000, 1000/60 = 16.6667. A GPU limit does not
land on 25.0077 ms twice. **The frame-time histograms are the clincher**: the full-rate run is a
broad 11–16 ms spread, the half-rate runs are narrow spikes (63.5% of one run inside a single
16–17 ms bucket). Broad spread = GPU-limited; narrow spike on exactly 2× the display period =
externally enforced pacing.

Confirmed independently the next morning: the *same DLL* and *byte-identical graphics settings*
produced 16.667 ms (locked) at 23:17 and 12.862 ms (free-running) at 08:04. Nothing in the code
changed between them.

**If a game reports exactly half refresh, check SSW in the Virtual Desktop Streamer before
anything else.** Note it engages when the app misses the frame budget, so it can appear to be a
code regression when it is really the app drifting over the line.

### The rolling frame-timing summary (`frameTimingSummaryFrames`, default 500)

Added because the hitch watchdog only reports frames *over* its threshold, which means the
steady-state frame — the one that actually sets your framerate — never appears in the log. One
line per N frames:

```
FRAMES 17998-18498: 80.0 fps of 80.0Hz (1.00x) | avg 12.50ms (min 10.15 max 14.63) | xrWaitFrame 6.60 | xrBeginFrame 0.11 | locateViews 0.01 | compositor 0.06 | xrEndFrame 0.07 | game 5.66
```

Read the ratio first. **Near 0.50x means a runtime-side half-rate lock** (SSW, SteamVR motion
smoothing) — quite different from merely being slow, which gives a ragged ratio. Safe to leave on;
at 60fps it is one line every ~8 seconds.

### What this instrumentation has already established, so don't re-derive it

- **OpenComposite is not the bottleneck in F1 25, by a wide margin.** Measured while driving:
  `compositor 0.06ms`, `xrEndFrame 0.07`, `xrBeginFrame 0.04`, `locateViews 0.00` — **0.17 ms
  total, about 1.2% of a 14.3 ms frame.** `game` is the entire rest.
- **The DX12 per-eye fence wait is a non-issue.** The swapchain image count is **3** (now logged),
  so the wait is on GPU work from three frames ago. A code review flagged it as the prime suspect
  for a frame-rate halving; measurement retired it. Don't restructure the allocator ring on a hunch.
- **The pipeline can hit full refresh.** In menus, `game` drops to ~5.7 ms, `xrWaitFrame` absorbs
  6.6 ms of slack, and the ratio is exactly 1.00x. Driving is game-limited, not shim-limited.
- **A USB link drop looks like this**: `xrEndFrame` jumps from 0.07 ms to 80–195 ms in one frame
  and stays pinned, alternating ~100/~200 ms, while `compositor` and `game` stay nominal. That is
  VDXR unable to ship the frame. Same family as the USB2 incident above.

### What F1 25 actually asks OpenComposite for

Established with `logGetTrackedProperty=true` over a full run. F1 25 requests **exactly one**
tracked-device property, once, during startup:

- `Prop_DisplayFrequency_Float` (2002)

That is the entire OpenVR property surface it uses. Consequences worth remembering:

- **The `hmd_name="Oculus Quest2"` in its benchmark XMLs does not come from us.** The game never
  asks who the headset is — not the model number, manufacturer, serial, or tracking system name.
  Editing `headsetName` in `hardware_settings_config_vr.xml` is pointless; the game rewrites that
  file on exit. Chasing this wasted a build. The label most likely comes from the
  `XR_APILAYER_VIRTUALDESKTOP_oculus_compatibility` layer or an engine default.
- **Display frequency has to be right before the first frame.** The game asks during startup, so
  deriving it from `XrFrameState::predictedDisplayPeriod` is too late — that is still zero and you
  silently serve the fallback. It now comes from `XR_FB_display_refresh_rate`
  (`xrGetDisplayRefreshRateFB`), which answers from session creation onward, with the frame-period
  route as fallback and 90.0 only if neither works. The startup log prints which source was used.

### OPEN: the hidden-area mesh is a single triangle

F1 25 *does* call `GetHiddenAreaMesh` at startup — and we return **1 triangle per eye**. A real
lens-occlusion mask is hundreds. The game evidently sanity-checks it, because it then writes
`stencilMesh="false"` into its VR config and keeps overwriting any manual edit. That is the game
correctly rejecting a broken mesh, not the game being stubborn.

Worth roughly 10–15% of GPU pixel work if it can be fixed. Unresolved as of 2026-08-16: the
logging now records both the capacity query's counts and the fill's counts, which distinguishes

- runtime returns a stub mask (nothing OpenComposite can do — VDXR limitation), from
- we mis-call the two-call idiom (our bug, real saving available).

Check `Hidden area mesh:` lines in the log to pick up where this left off.

## Current state

**Working:** F1 25 and Automobilista 2, both via VirtualDesktopXR, no SteamVR. At 2496x2152 per
eye (10.7 Mpixel/frame) on a 3060 Ti, F1 25 runs ~70–80 fps while driving and hits the display cap
exactly (1.00x) in menus. The headset is a **Meta Quest 3** — the "Quest2" in F1 25's own files is
a mislabel from elsewhere, see above.

**F1 25 is DX12.** AMS2 is DX11. Relevant because the DX11 and DX12 paths differ a lot, and the
DX12 path has no MSAA support at all.

### Verified by running

The DX11 copy path, DX12 `CopyResource` path (now with resource barriers), session lifecycle,
interface registration, the `GetFrameTiming` bound, the logger mutex, the rolling frame-timing
summary, and `XR_FB_display_refresh_rate`.

### NOT verified — check these first if a game misbehaves

- **DX11 state save/restore** — needs `invertUsingShaders=true`; neither game exercised it
- **DX12 bounds copy** (`CopyTextureRegion`) — only runs when bounds are non-null, which needs
  `invertUsingShaders=true`
- **DX12 array-texture slice copy** — needs a game submitting a 2-slice stereo texture
- **Hand-tracking fallback** — needs controllers set down mid-session
- **MSAA resolve** (DX11 and Vulkan) — neither game submitted multisampled textures
- **Anything Vulkan** — no Vulkan title tested
- **32-bit build** — compiles and links, never run

### Performance

Measured, don't guess. **The shim costs 0.17 ms of a 14.3 ms frame.** There is essentially nothing
left to win in this codebase for F1 25 — see the instrumentation findings above before proposing
any optimisation.

F1 25 calls `GetFrameTiming` **once**, so the "feed real frame timings to fix dynamic resolution"
idea is **dead**. Remaining code-level wins (per-frame pose cache, MSAA resolve-direct-to-swapchain)
are well under a millisecond on a frame that already finishes early.

**The levers are all outside the code**, in rough order of value:

1. **SSW off** in the Virtual Desktop Streamer, if the ratio in the FRAMES line sits near 0.50x.
2. **Headset refresh rate** in Virtual Desktop.
3. **`supersampleRatio`** in the ini — and it doubles as the definitive CPU-vs-GPU test: drop it
   substantially and re-run the benchmark. fps rises in proportion → GPU-bound; fps barely moves →
   CPU-bound, and no pixel-side setting will help.
4. **The game's own extra scene renders** — wing mirrors, cube-map reflections, and
   `cs_culling` (off by default in F1 25's VR config, which leaves culling on the CPU).

**Beware Task Manager's aggregate numbers.** On a 16-thread CPU, one fully saturated thread is
6.25% of the total — "25% CPU" is entirely consistent with being single-thread bound. GPU
utilisation is "any work in flight", not saturation, so 70–80% is not 20–30% of headroom. Neither
number can detect this bottleneck; the supersample test can.

OpenComposite has no FSR/upscaling and shouldn't — use OpenXR Toolkit, which layers *after*
OpenComposite in the OpenXR chain. Note `openvr_fsr` is incompatible: it also replaces
`openvr_api.dll`.

---

## Deployment

```bash
cp build/bin/Release/vrclient_x64.dll "<game>/openvr_api.dll"     # note the rename
```

- F1 25: `F:\Programs\Steam\steamapps\common\F1 25\` (backup: `openvr_api.dll.backup`, Valve's original)
- AMS2: `F:\Programs\Steam\steamapps\common\Automobilista 2\x64\` (`openvr_api.dll.backup` is stock
  OpenComposite, `openvr_api-orig.dll` is Valve's)
- Known-good build kept at `build/openvr_api-KNOWN-GOOD.dll`

The F1 25 folder has accumulated `openvr_api opencomposite backup N.dll` files from successive
test builds. They are inert — nothing loads a DLL by those names — but only two are meaningful
rollback points: `openvr_api.dll.backup` (Valve's) and `openvr_api opencomposite backup 2.dll`
(the last pre-review build). The rest can go.

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

**The 2026-08-16 session repeated the same lesson three times.** Each wrong turn was a plausible
inference; each was settled by one log line:

- *"The DX12 per-eye fence wait is halving the framerate."* A reasonable read of the code — DX12 is
  the only backend that blocks the CPU on the GPU. Wrong: the swapchain has 3 images and the
  compositor costs 0.06 ms. Logging `imageCount` and a per-phase average retired it.
- *"The wrong headset model is why the game disables its stencil mesh."* Wrong twice over — the
  game never asks us for the model, and the real cause was the 1-triangle mesh we hand it.
  `logGetTrackedProperty` showed F1 25 queries exactly one property, full stop.
- *"The counter showing half FPS must be a reporting bug in our frame timing."* Wrong: the counter
  was honest and the app really was running at half rate. The user's own benchmark XMLs proved it
  in minutes, without a build.

Also worth internalising: **evidence the user already has often beats new instrumentation.** The
benchmark XMLs, the frametimes CSVs, and the existing log answered more questions here than any
code change did. Look at what's on disk before writing anything.
