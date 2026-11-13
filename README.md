# FrameSmooth

**Disclaimer: This is heavily vibecoded so use on your own risk**

**Half-rate rendering with motion reprojection for Steam Frame on Windows**, similar in spirit to Oculus ASW
or SteamVR Motion Smoothing. It is a separate add-on that you can remove completely.

A VR game renders at half the headset's refresh rate (for example 45 fps at 90 Hz). FrameSmooth fills in
every second frame by estimating the motion between the last game frames on the GPU and extrapolating the
newest frame forward in time. It also corrects head rotation itself before each frame is shown.

It was built for and tested with a **Steam Frame** used as a PCVR headset through SteamVR, but it does not
contain anything headset-specific.

> **Status: experimental.** It works well in the games it has been tested with (see below), but every game
> engine is different. Use it at your own risk. The safety features described below make it fall back to
> normal rendering when something goes wrong.

## Features

- **Frame rate modes**
  - **Auto:** native rate while the game keeps up; half rate only when it can't.
  - **Always half:** the game renders at half the refresh rate, and every second frame is synthesized.
  - **Native:** never forces half rate, but still fills in frames the game delivers late.
- **Reprojection:** *Motion* (ASW-like, GPU motion estimation) or *Rotation only*.
- **SteamVR dashboard tab** for all settings and live status (game fps, synthesized frames per second, API).
- **Tint fake frames** option: synthesized frames are tinted green, so you can see them.
- **Exit button**, Start/Exit shortcuts, a `--toggle` option for a Steam library shortcut, and an optional
  *Start with SteamVR*.

## Supported games

| Game API | Graphics API | How |
|---|---|---|
| OpenXR | Direct3D 11, Direct3D 12 | automatic (OpenXR API layer) |
| OpenVR | Direct3D 11, Direct3D 12, Vulkan | per-game opt-in in the dashboard tab |
| any | OpenGL | not supported (the game runs normally) |

Tested so far: *EXD* (OpenXR, D3D11) and *No Man's Sky* (OpenVR, Vulkan).
Reports for other games are welcome. Please open an issue and attach the logs mentioned below.

## Safety

- **Nothing is changed in Steam, SteamVR, the headset or any game.** See *What gets installed*.
- **No effect while FrameSmooth is closed.** The OpenXR layer is registered as *disabled* and is only
  enabled while the FrameSmooth app runs. The OpenVR hook is only loaded into games you enable yourself.
- **Anti-cheat games are left alone.** FrameSmooth detects EasyAntiCheat, BattlEye and similar next to a
  game. It never loads into such games, and it disables its OpenXR layer while one of them is running.
  Loading code into a game with anti-cheat could get you kicked or banned.
- **Freeze protection.** If FrameSmooth stops receiving frames for 2 seconds, or a game waits more than
  1 second for it, rendering is handed back to the game.

## Installation

Requirements: Windows 10/11 (64-bit), SteamVR, and a GPU with Direct3D 11.4 support (any current GPU).

1. Download `FrameSmooth-<version>.zip` from the [Releases](../../releases) page and extract it.
2. Double-click **`Install.cmd`**. No admin rights are needed.
3. Start SteamVR and open the dashboard. A **FrameSmooth** tab appears.

Windows SmartScreen or your antivirus may warn about the download. The files are not code-signed, and the
OpenVR hook uses DLL injection, which heuristic scanners flag. You can build the files yourself from this
source code instead (see below).

### What gets installed

All of it is removed by `Uninstall.cmd` (also found in the install folder):

- `%LOCALAPPDATA%\Programs\FrameSmooth\` (program files)
- `%LOCALAPPDATA%\FrameSmooth\` (settings and logs)
- One registry value under `HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` (the OpenXR layer
  registration; set to *disabled* whenever FrameSmooth is not running)
- Start Menu shortcuts *FrameSmooth* and *Exit FrameSmooth*, plus an optional Startup shortcut
  (*Start with SteamVR*)

## Usage

- **OpenXR games:** just start the game while FrameSmooth is running.
- **OpenVR games:** start the game, open the FrameSmooth tab and press **Enable for this game**. This is
  remembered; from then on FrameSmooth attaches about 10 seconds after the game starts in VR.
- **Exit:** use the **Exit** button in the tab, the *Exit FrameSmooth* shortcut, or run
  `FrameSmoothDashboard.exe --exit`.
- **Steam library shortcut:** *Games → Add a Non-Steam Game*, select
  `%LOCALAPPDATA%\Programs\FrameSmooth\FrameSmoothDashboard.exe` and set its launch options to
  `--toggle`. Each press of *Play* then starts FrameSmooth or closes it.
- If a game looks wrong, press **Off** next to *This game*, or switch *Frame rate* to *Native*.

### Logs

`%LOCALAPPDATA%\FrameSmooth\layer.log` (OpenXR), `openvr_hook.log` (OpenVR) and `watch.log` (layer
registration and anti-cheat detection).

## How it works

- **OpenXR:** an implicit OpenXR API layer creates the session on its own D3D11 device and gives the game
  shared textures. A separate thread drives the real frame loop at the display rate and releases the
  game's `xrWaitFrame` every N-th frame.
- **OpenVR:** opted-in games get `FrameSmoothOVR.dll` loaded into their process. It patches the game's
  `IVRCompositor` interface tables in memory (interface versions 019–029) and virtualizes the frame loop
  the same way. For Vulkan games, frames are shared with D3D11 through external memory and handed back
  to SteamVR as Vulkan images. All use of the game's Vulkan queue is serialized with a small per-call
  lock (MinHook).
- **Motion engine** (D3D11 compute shaders): luma pyramid → head-rotation compensation → coarse and fine
  block matching → median filter → backward warp, combined with rotational reprojection to the display
  pose.

## Building from source

Requirements: Visual Studio 2022 (or Build Tools) with the C++ workload and CMake.

```
powershell -ExecutionPolicy Bypass -File build.ps1 -BuildDir C:\fsbuild
powershell -ExecutionPolicy Bypass -File package.ps1 -BuildDir C:\fsbuild
```

The dependencies (OpenXR SDK, OpenVR SDK, Vulkan-Headers, MinHook) are downloaded by CMake. Use a short
build path, because MSBuild fails with paths longer than 260 characters. `package.ps1` creates the release
zip in `release\`.

`tools/submit_test.cpp` is a developer test program (a fake OpenVR/Vulkan VR game). It is not part of the
release.

## License

FrameSmooth is released under the [MIT License](LICENSE). Third-party components and their licenses are
listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

FrameSmooth is an independent project. It is not affiliated with, endorsed by or sponsored by Valve,
Meta, Hello Games or any other company. Steam, SteamVR and Steam Frame are trademarks of Valve
Corporation; other names are trademarks of their respective owners and are used only to describe
compatibility.
