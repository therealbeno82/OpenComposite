# OpenComposite — ready-to-use DLL (no compiling required)

This folder contains a **pre-built OpenComposite DLL** so you don't have to install Visual Studio
and build it yourself. Download it, copy it into your game folder, done.

OpenComposite lets SteamVR games run directly on your headset's OpenXR runtime — **SteamVR never
launches**.

> This is a personal modified build, mainly tested on **F1 25** and **Automobilista 2** with a
> **Meta Quest 3 over Virtual Desktop**. It is not an official OpenComposite release. Official
> upstream is <https://gitlab.com/znixian/OpenOVR> — please take general OpenComposite bugs and
> support questions there, not here.

---

## Before you start

You need:

- **Windows**, 64-bit. (This DLL is x64 only. F1 25 and AMS2 are both 64-bit, so that's fine.)
- **A working OpenXR runtime for your headset.** This is whatever software already streams VR to
  your headset:
  - Quest over **Virtual Desktop** → set the OpenXR runtime to **VDXR** in the Virtual Desktop
    Streamer settings on your PC.
  - Quest Link / Air Link → the Oculus runtime.
  - Pico, WMR, etc. → that vendor's runtime.
- **Your OpenXR runtime must NOT be set to SteamVR.** If it is, OpenComposite just hands everything
  straight back to SteamVR and you gain nothing. In Virtual Desktop Streamer, that setting is
  literally a dropdown — make sure it says VDXR.

You do **not** need SteamVR installed or running.

---

## Install — F1 25

The file in this folder is **already named `openvr_api.dll`**, so there is nothing to rename. You
are replacing the game's copy of that file with this one.

**1. Open the game folder.**

In Steam, right-click **F1 25** → **Manage** → **Browse local files**. A folder opens containing
`F1_25.exe`. It's typically:

```
C:\Program Files (x86)\Steam\steamapps\common\F1 25\
```

(It'll be on whichever drive your Steam library lives on.)

**2. Back up the original. Don't skip this.**

In that folder, find `openvr_api.dll` and rename it to:

```
openvr_api.dll.backup
```

That's Valve's original file — it's your one-click undo if anything goes wrong. It's about 820 KB;
the OpenComposite one is about 2.5 MB, which is a handy way to tell them apart later.

**3. Copy this folder's `openvr_api.dll` into the game folder.**

**4. Launch.**

Start your VR streaming software first (e.g. Virtual Desktop), put the headset on, then launch
F1 25 from Steam and choose VR mode as usual.

SteamVR should never appear. If it does, see [Troubleshooting](#troubleshooting).

---

## Install — other games

Same three steps. The only difference is *where* `openvr_api.dll` lives — always the folder
containing the game's `.exe`:

| Game | Folder |
|---|---|
| F1 25 | `steamapps\common\F1 25\` |
| Automobilista 2 | `steamapps\common\Automobilista 2\x64\` |

For anything else, search the game's install folder for `openvr_api.dll` and replace the one next
to the main executable. A 64-bit game needs this DLL; 32-bit games need a 32-bit build, which
isn't included here.

> **Games overwrite this file when they update.** After any game patch, VR may go back to
> launching SteamVR — that just means the update restored the stock `openvr_api.dll`. Redo the
> copy. (Verifying game files in Steam will also undo it, which is exactly why that's the
> uninstall method below.)

---

## Uninstall / go back to SteamVR

Either:

- Delete `openvr_api.dll` from the game folder and rename `openvr_api.dll.backup` back to
  `openvr_api.dll`; **or**
- In Steam, right-click the game → **Properties** → **Installed Files** → **Verify integrity of
  game files**. This restores the original automatically.

---

## Did it work?

Two quick checks:

- **SteamVR didn't launch** when the game started in VR. That's the whole point.
- **A log file appeared** at:

  ```
  %LOCALAPPDATA%\OpenComposite\logs\opencomposite.log
  ```

  Paste that path into the Explorer address bar. Near the top there's a `Runtime:` line naming
  your OpenXR runtime — it should say your streaming runtime (e.g. VDXR), **not** SteamVR.

That log is also the first thing to look at if something misbehaves, and the first thing to attach
if you report a problem.

---

## Optional settings (`opencomposite.ini`)

Entirely optional — the defaults are fine. To change something, create a plain text file named
`opencomposite.ini` **in the same folder as the DLL** (i.e. the game folder) containing:

```ini
[Config]
supersampleRatio=1.0
```

Settings worth knowing about:

| Setting | Default | What it does |
|---|---|---|
| `supersampleRatio` | `1.0` | Render resolution multiplier. `0.8` for more speed, `1.2` for more sharpness. The single biggest performance lever here. |
| `hitchWarningMs` | `30` | Log a one-line breakdown of any frame slower than this, naming which stage was slow. Cheap — a healthy frame logs nothing. |
| `frameTimingSummaryFrames` | `500` | Log an average framerate/timing summary every N frames. Set `0` to turn off. |
| `renderCustomHands` | `true` | Draw generic hands when the game doesn't supply controller models. |
| `haptics` | `true` | Controller vibration. |
| `invertUsingShaders` | `false` | Alternate image-flip path. Only try this if the picture is upside-down or mirrored. |
| `initUsingVulkan` | `false` | Try `true` only if a Vulkan game fails to start. |
| `logAllOpenVRCalls` | `false` | **Leave this off.** Useful for diagnosing a crash, but it has produced a 970 MB log file in a single session. |

---

## Troubleshooting

**SteamVR still launches.**
Three usual causes, in order of likelihood: your OpenXR runtime is set to SteamVR (fix it in
Virtual Desktop Streamer / your runtime's settings); you copied the DLL into the wrong folder; or
a game update replaced it. Confirm the `openvr_api.dll` in the game folder is ~2.5 MB, not ~820 KB.

**The game shows an "OpenComposite Error - info in log" box, or won't start.**
Open `%LOCALAPPDATA%\OpenComposite\logs\opencomposite.log`. The last few lines usually name the
problem outright. Note that only the previous run is kept, as `opencomposite.log.1` — copy the log
somewhere else before relaunching if you want to keep it.

**Framerate looks like exactly half the headset's refresh rate** (e.g. 45 fps on a 90 Hz headset).
That's almost always **Synchronous SpaceWarp** in the Virtual Desktop Streamer, not a fault — it
halves the render rate and synthesises the in-between frames. Turn SSW off in the Streamer if you
don't want it. Check that before assuming anything is broken.

**Stuttering, or generally poor performance.**
Check the physical link first — a Quest that has negotiated **USB 2** instead of USB 3, or a weak
Wi-Fi link, will stutter no matter what any software setting says. This has masqueraded as a
software bug here before. After that, lower `supersampleRatio` or the headset refresh rate.

**Missing menu elements or UI in F1 25.**
A couple of newer overlay features have no OpenXR equivalent and are stubbed out in this build. It
shouldn't crash, but some UI may not appear.

---

## A note on anti-cheat

**F1 25 ships EA Javelin anti-cheat.** Replacing a DLL inside a game protected by anti-cheat
carries some inherent risk. In testing here it caused no problems and there's no sign it
interfered — but that isn't a guarantee, and nobody has formally cleared it. Keep your backup, and
make your own call. The same caution applies to any anti-cheat-protected title.

---

## What this build is

See [`../CLAUDE.md`](../CLAUDE.md) for the full detail. In short, on top of upstream OpenComposite:

- **F1 25 support** — added the `IVROverlay_028` / `IVRSystem_023` interfaces the game demands at
  startup, and fixed a crash where the game passed a ~1.9 GB uninitialised buffer size into
  `GetFrameTiming`.
- **A batch of bug fixes** — DX12 resource barriers and correct per-eye texture slices, several
  memory-corruption and out-of-bounds fixes, a logger data race, and exceptions that were escaping
  through the OpenVR C ABI into the game.
- **Frame-timing instrumentation** — the hitch watchdog and rolling summary described above.
- **Correct display refresh rate** reported to the game, via `XR_FB_display_refresh_rate`.

Known limitation: the hidden-area (lens occlusion) mesh is a stub, so F1 25 disables its stencil
mesh optimisation. Costs some GPU time; nothing breaks.

Verified working: F1 25 (DX12) and Automobilista 2 (DX11) via VirtualDesktopXR on a Quest 3.
Untested: Vulkan games, MSAA-submitting games, and the 32-bit build.

## Build provenance

So you can confirm you have the file you think you have:

| | |
|---|---|
| File | `openvr_api.dll` (built as `vrclient_x64.dll`, renamed for drop-in use) |
| Size | 2,550,272 bytes |
| SHA-256 | `346626441641e19d74249c3f6eb0cdd0bfe0ca8a76c07020e23fc538379c8fd8` |
| Built from commit | `c219a74` |
| Built | 2026-08-18, MSVC 14.44, Release x64 |

Check it in PowerShell with:

```powershell
Get-FileHash .\openvr_api.dll -Algorithm SHA256
```

Prefer to build it yourself? See [`../CLAUDE.md`](../CLAUDE.md) — note that the dependencies in
`libs/` are hand-installed and are not in this repo.

## Licence

GPLv3, same as upstream OpenComposite. The complete source for this DLL is the repository this
folder sits in.
