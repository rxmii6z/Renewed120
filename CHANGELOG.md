# Changelog

## v0.82

Two fixes for menus found after v0.81.

### Fixed
- **Shop menus racing down the list** (clothes, Ammu-Nation and the other shops): one press moved the highlight
  several lines, as if the button was held, and the menu sound played two to six times. These menus read the buttons
  on every drawn frame, but the mod only refreshes the controller, keyboard and mouse on simulation steps, so one
  press counted on every frame of its step. It now counts once.
- **Map of the pause menu hardly zooming with the mouse wheel** at a high frame rate: the game only takes a wheel
  notch on a frame where more than 20 ms passed since the last one, so above 30 fps most notches were lost (four in
  five at 240 fps). This is the original game's, shown by the unlocked frame rate. Notches are now kept until the map
  takes them.

## v0.81

Two fixes found after v0.8.

### Fixed
- **Car camera slow to swing back behind the vehicle** with the camera on every frame: after a turn it came round
  about 3.6 times too slowly at 180 fps and had to be helped with the mouse. The game turns the angle the camera wants
  this step into a turn speed by dividing it by the time step, but never by less than 1: right at 30 fps, too small
  when the camera runs on every drawn frame. The camera behind CJ on foot swung back too slowly for the same reason.
- **CJ's arms shaking on a bicycle**: the game bends his arms so his hands hold the handlebar once per simulation
  step. Between steps the arms went back to the animation's pose (up to 10 cm away when riding), 30 times a second.
  The hands are now put on the handlebar on every drawn frame.

## v0.8

The camera on every frame is now on by default, with the fixes found since v0.7.

### Changed
- **Camera on every frame on by default** (`CameraEveryFrame=1`, no longer experimental): the view answers the mouse or
  the stick on the next drawn frame instead of one simulation step late (33 ms). Set `CameraEveryFrame=0` in
  `Renewed120.ini` (or turn it off in the developer menu, F9) to get the blended camera of v0.6 back.

### Fixed
- **Aiming with a pistol, Uzi, Tec-9 or sawn-off**: CJ kept facing one way while the camera turned, and walking while
  aiming went wrong. With these weapons the game turns him itself (the camera only does it for weapons held at the
  shoulder), and the fix for the aiming shake of v0.7 undid that turn on every frame.
- **Climbing**: the camera rose by steps 30 times a second during the climb (it aims at a point that follows the
  climb animation, which only moves on simulation steps).
- **Climbing**: a jerk of the camera when the climb starts and when CJ gets up on top (the camera went away and came
  back).
- Safer while a save is loaded: the camera on every frame no longer reads the player unguarded while he is rebuilt.

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
