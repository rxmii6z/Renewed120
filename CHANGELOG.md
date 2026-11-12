# Changelog

## v0.7

VSync, a camera without lag, the last things that still moved 30 times a second, and a pass over the timing of
every drawn frame.

### Added
- **VSync** (`VSync=1`): no tearing. With `FpsCap=0` or `-1` the frame rate follows the monitor. It goes through the
  game's own setting (RenderWare asks Direct3D to wait for the refresh), in borderless and exclusive fullscreen, and
  works with DXVK.
- **Camera on every frame** (`CameraEveryFrame=1`, experimental, off by default): the game's camera runs on every
  drawn frame instead of being shown one simulation step late, so the view answers the mouse or the stick on the next
  frame (no more 33 ms lag).
  - Only for the views you steer: on foot, behind a car, a bike or a boat, and aiming (weapons, sniper, rocket
    launchers, from a car).
  - The cinematic, idle (AFK), first-person, fixed and scripted mission cameras, death / arrest and cutscenes keep
    the blended camera: some of them move a fixed amount each time they run and would shake six times too fast.
  - Mouse movement read on every frame and added up for the simulation, so aiming and driving get all of it once.
  - A button pressed on a simulation step counts once (change of view, look behind...), not once per drawn frame.
  - Fades (walking through a door, entering an interior) keep running while the screen is black.
- **Lights and shadows of the simulation on every frame** (`LightsEveryFrame=1`), see Fixed.
- New developer menu (F9) entry for the camera.

### Changed
- **`Renewed120.ini` rewritten**: grouped in display, simulation, details and developer, one short line per option.
  Same options and defaults as before, plus `VSync`, `CameraEveryFrame` and `LightsEveryFrame`. Replace your old
  file (or copy your values over).
- With VSync, a frame cap at or above the monitor's rate is ignored (two pacers fought each other).

### Fixed
#### Things that still moved 30 times a second
- **Moving 3D markers**: the cone bobbing in front of a door, turning arrows, pulsing rings of mission markers.
- **Ropes**: crane, magnet and helicopter winch.
- **Animated objects**: the basketball in CJ's hands and other props playing an animation.
- **Lights of fires, explosions and gunshots**, and the lights and shadows drawn by mission scripts: they were only
  there on one drawn frame per simulation step (one out of six at 180 fps), so they looked dim and flickered.

#### Frame timing
- **Small shake of the car against the camera** without a frame cap: everything in a frame is now blended for the
  same moment (the time was read again at each step of the frame).
- **Waves, pickups and birds** took one short step then one long step 30 times a second: the frame of the simulation
  step is moved ahead like the others.
- **Uneven steps with a frame cap** when the frame and simulation schedules were close: the simulation step could
  run half a frame early.
- With VSync, each frame is blended for the refresh it is shown on (the frames of a simulation step were blended
  late).

#### Camera on every frame (fixed during this release's testing)
- **Fast left-right shake while aiming**: the aiming camera follows CJ's heading and a bone of his back, which the
  simulation step put back a few degrees and one step ahead. It now always starts from where it left them.
- **CJ moving 30 times a second when running while aiming**: the camera turns him to where he aims, far from the way
  he runs, and the blend took that for another mod moving him (teleport guard).
- The view no longer jumps once when going from the live camera back to the blended one (door, cinematic view): the
  mouse movement was given to the camera twice.

#### Stability and compatibility
- **Developer menu (F9) not opening** when another mod creates its own hidden Direct3D device (controller mods such
  as DualSense support): that window was taken for the game's.
- **Physics guard** (vehicles and peds kept untouched by the extra frames): it told pointers from numbers by their
  value alone, which stops working once the game uses a lot of memory (DXVK, limit adjusters). It now learns which
  fields hold pointers and never puts those back.
- The log says so when a limit adjuster makes a pool larger than what the mod blends.

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
