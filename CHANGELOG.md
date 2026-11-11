# Changelog

## v0.6

More things that still moved at 30 fps, and a crash when loading a save.

### Fixed
- **Teleport** (CheatMenu and other mods/scripts that move the player or a car): the car or player is no longer put
  back where it was one frame later.
- **Weapons and pickups on the ground** (health, armour, money...) really spin and glow smoothly now: v0.5 only
  covered a few special pickup models.
- **Crash when loading a save** (mostly at a high or uncapped frame rate), and the player staying where they were
  instead of the saved place: the blend wrote skeletons and positions kept from before the load into the rebuilt
  world. Everything kept for the blend is now forgotten while the menu is open, and a skeleton is only written back
  if it is still the same one.
- Objects made of one piece were looked up as if they had a skeleton, reading random memory.
- **Mirrors and reflective floors** in interiors follow the camera on every frame (the reflection moved at 30 fps).
- **Moving fog** drifts at the game's speed whatever the frame rate: it moved once per drawn frame, so 10 times too
  fast at 300 fps. New ini option `SmoothFog`.
- **Camera zoom** (field of view: idle camera, aiming, vehicle cameras) changes smoothly instead of at 30 fps.
- **HUD flicker once a second** while it should be hidden (idle camera...): the water fix left the frame counter one
  ahead for the rest of the frame, once every 32 ticks.
- **Birds** fly and flap their wings smoothly.
- **Police helicopter searchlights** no longer flicker (one more copy of the light was added on every drawn frame
  between two ticks) and the beam sweeps smoothly.
- **Peds whose head was blown off** fall smoothly: the shrunk head bone made the whole skeleton fall back to the
  30 fps pose.
- **Broken windscreen and window pieces** fall and spin smoothly.
- **Water jets** of fire trucks and SWAT tanks flow smoothly:
  - the jet follows the camera when it turns (it was turned to the camera of the last tick);
  - its end no longer grows, fades and snaps back in steps 30 times a second;
  - the ribbons that give it its volume change direction 30 times a second like in the original game, not on every
    drawn frame (the end looked doubled);
  - parts of the jet no longer blink on a moving truck.
- **Sky gradient** no longer flickers when the camera moves fast (falling under the map...): the sky was placed
  from the camera of the last tick.
- **Money dropped by peds** and the collectables (tags, horseshoes, oysters, snapshots) spin smoothly.
- **Props held by peds** (bottle, cigarette, box...) follow the hand on every frame: they were put on the hand of
  the 30 fps skeleton.

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
