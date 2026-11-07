# Renewed120

ASI plugin for **GTA San Andreas 1.0 US** (work in progress, part of the GTA SA Renewed project).

GTA SA ties its whole simulation to the frame rate: above 30 fps physics, animations and many effects start to misbehave.
Renewed120 separates the two. The world is simulated at a fixed rate (30 Hz by default) exactly like the original
game, and every frame drawn in between shows a smooth blend of the last two simulated states. The result is a high
frame rate (as high as your monitor) without breaking the game.

## Features

**Display**
- uses the monitor's refresh rate instead of the fixed 60 Hz (`RefreshRateFix`, `ForceHz`)
- borderless windowed fullscreen, no mode switch or black flash (`WindowMode`)
- starts at the resolution of your desktop (`DesktopResolution`)
- the game's own 30 fps frame limiter is turned off (`NoFrameLimiter`)
- precise frame cap: automatic (monitor refresh rate), a fixed value or no cap (`FpsCap`)

**Simulation / rendering split**
- game logic at a fixed rate (`LogicHz`, 30 by default), each step simulating exactly the same time (`FixedStep`)
- interpolation between two simulation steps (`Interpolate`) of:
  - the camera, in game and in cutscenes (camera cuts are detected and not blended)
  - vehicles and their parts: wheels, doors, suspension, bike handlebars and forks (`VehicleParts`)
  - peds and their skeleton animations, with smooth real-time shadows (`PedSkeleton`)
  - objects and cutscene characters with the props they hold (`CutsceneSkeleton`)

**Fixes for the game systems that expected one drawn frame per simulation step**
- water drawn and animated on every frame, no blinking sea (`WaterFix`)
- vehicle and ped physics untouched by the extra drawn frames: no rear axle wobble, no icy sand (`VehicleGuard`)
- blood pools and other fixed shadows kept between steps (`StaticShadowFix`)
- particles (smoke, sparks...) and broken object pieces animated on every frame (`SmoothFx`)
- headlight patches of AI cars on the ground recomputed every frame (`SmoothCarLights`)
- car shadows follow the cars on every frame, both the simple shadows (low visual FX) and the shadow volumes (medium and above) (`SmoothCarShadows`)
- the sun and the shadows it casts move smoothly as the time of day passes (`SmoothSun`)
- map models and LODs loaded from disk as fast as in the original game (`StreamEveryFrame`)

**Performance**
- sounds updated once per simulation step like the original game, on the frame after the step, instead of on every drawn frame: 1-2 ms saved per frame in dense traffic (`AudioAtLogicRate`)

**Developer tools**
- in-game developer menu on **F9**: toggle every feature live and save the settings to the ini (`DevMenu`)
- log file (`Log`) and optional telemetry CSV with FPS, frame times, the time of each step of the frame and the time spent in the mod (`Telemetry`, off by default)

Every option is explained in [Renewed120.ini](Renewed120.ini).

## Requirements

- GTA San Andreas **1.0 US** exe (the memory addresses are for that version only)
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
- works alongside SilentPatch and DXVK (d3d9)

## Install

Download the zip from the [releases](https://github.com/rxmii6z/Renewed120/releases) and extract `Renewed120.asi` and
`Renewed120.ini` next to `gta_sa.exe`.

## Video

Short video presenting the project: https://www.youtube.com/watch?v=uPssjMR5gXg

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

The plugin is written to `build/Release/Renewed120.asi`.

No game files are included in this repository.

## License

GPL-3.0. You can reuse and modify this code, but derived work must stay open source under the same license. See [LICENSE](LICENSE).
