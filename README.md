# Renewed120

ASI plugin for **GTA San Andreas 1.0 US** (work in progress, part of the GTA SA Renewed project).
It decouples the game logic from rendering to reach a smooth high frame rate:

- game simulation fixed at 30 Hz, rendering up to 180+ fps (`LogicHz`, `FpsCap`)
- interpolation of camera, vehicles, peds (skeleton bones) and objects between simulation ticks
- refresh rate fix (uses the monitor's refresh rate) and borderless windowed mode (`WindowMode`)
- fixes for systems that assumed one render per simulation step: water, wheel rotation, vehicle/ped state guard, blood pools
- in-game developer menu (F9) to toggle every feature live
- optional telemetry (`Telemetry=1`)

Needs [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) and works alongside SilentPatch and DXVK (d3d9).
The offsets are for the 1.0 US exe only.

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Copy `build/Release/Renewed120.asi` and `Renewed120.ini` next to `gta_sa.exe`.

No game files are included in this repository.
