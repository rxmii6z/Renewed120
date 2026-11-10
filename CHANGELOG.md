# Changelog

## v0.5

Everything that still moved at 30 fps while the rest of the game runs at your monitor's rate.

### Fixed
- **3D markers** (entrance markers of buildings, red mission rings and arrows) are visible again: they
  disappeared on the frames drawn between simulation steps (issue #1).
- **Sea waves** move smoothly. Before, their height only changed 30 times per second.
- **Weapons and pickups on the ground** (health, armour, money...) spin and glow smoothly.
- **Bike, motorbike and BMX shadows** no longer split into two faint shadows that drift further apart the faster you
  ride. At medium visual FX and above, the shadow was built from the bike's lean of an earlier frame.
- **Car sparks** (crashes, scraping a wall, rims on the road) come out as a steady stream instead of 30 bursts per
  second:
  - the sparks of each simulation step are spaced evenly over the drawn frames;
  - while waiting, a spark stays attached to the car as drawn, so the stream follows the car exactly, even at speed;
  - wheel sparks no longer pile up 6 times on the same spot;
  - sparks no longer jump ahead at the start of each step.
- **Sparks stretched when turning the camera fast**: their motion streak now covers one drawn frame of camera
  movement, instead of everything the camera did since the last simulation step.

### Changed
- Developer menu (F9) reorganised by theme. The test options are gone.
- New ini options:
  - `SmoothClock`: waves and pickups;
  - `SmoothSparks`: spread out sparks and other effects;
  - `CarStencilEachFrame`: car, bike and BMX shadows at medium visual FX and above. It is now separate from
    `SmoothCarShadows`, which only covers low visual FX.

## v0.4
- Sounds updated once per simulation step (on the frame after it), 1-2 ms saved per frame in dense traffic.
- Per-step frame profiler in the telemetry.

## v0.3
- Car shadows follow the cars on every frame (blob and stencil shadows).
- Sun direction and sun shadows move smoothly.

## v0.2
- Cutscene skeletons fixed, desktop resolution at startup, the game's own frame limiter turned off.
