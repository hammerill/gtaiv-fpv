# GTA IV FPV

![GTA IV FPV GIF](gtaiv-fpv.gif)

Fly an FPV drone through Liberty City with your own radio. Press F7 and Niko
launches a quad from the street in front of him. It is simulated by 
[libfpv](https://github.com/hammerill/libfpv), which
runs real Betaflight firmware, so rates, angle mode, air mode, arming and
turtle mode behave as they do on a real quad.

## Features

- **Real Betaflight** in the loop, with your rates, modes and tuning. Set up
  your radio once in the F8 overlay's wizard. Betaflight Configurator can
  connect too.
- **Three cameras**: FPV (the goggles view, with a fisheye lens), chase and
  line of sight from where Niko stands. In chase and line of sight you see
  the quad itself, props spinning (or broken).
- **Real goggles**: libfpv's video feed (F8 > Camera > Video feed) with
  analog noise, sparklies, torn and rolling lines and snow as the signal
  fades behind buildings, the camera's resolution and 4:3 picture with black
  bars, and the OSD written into the signal. Needs `d3dcompiler_47.dll`
  (part of Windows 10/11); without it the plain fisheye view is used.
- **Back to Niko (F)**: the view goes back to Niko while the drone stays in
  the world, still flown by the radio. Walk or drive; F7 goes back to the
  drone. The drone sound is heard from Niko then.
- **Niko stands in for the drone**: with the stand-in on (F8 > Fly > Pilot,
  the default), Niko goes along with the drone, hidden, for the whole
  flight, so the city streams around it and the police and gangs see, chase
  and shoot it. What hurts him breaks props. Nobody stays at the launch
  spot: GTA IV can't draw a script-made copy of Niko (it crashes the game),
  and the stand-in distances in that menu aren't used. With the stand-in
  off, Niko stays where you launched, head down over his phone (the "radio")
  whenever you touch the sticks.
- **Wind** from the weather (calm when sunny, a storm in thunderstorms),
  sheltered by buildings, plus helicopter downwash and explosion blasts.
- **Water, fire, explosions and rotors** break props; the drone sinks into
  water instead of bouncing off it.
- **Betaflight OSD** in the FPV view: battery, cell voltage, current, flight
  timer and mode. The GPS elements work too (speed, altitude, vario, distance
  flown, home arrow and distance); turn them on in Configurator's OSD tab.
  Home is Niko, so the arrow always points back to him.
- **Collides with the world**: buildings, the road, cars, peds and props, all
  through the engine's own physics probe. Crashes break props and burn
  motors. It can perch on a car roof and ride along.
- **Crash effects**: a fast hit knocks a ped over (ragdoll and damage), dents
  a car or shoves a prop.
- **Drone sound** from libfpv: motors and Betaflight's beeps, in stereo,
  onboard in FPV and chase, heard from Niko (distance, walls, Doppler) in
  line of sight and the F view; F8 > Camera & sound changes it.
- **Clean view**: the minimap, HUD, help boxes and subtitles are hidden
  while you fly; the only thing over the picture is Betaflight's OSD.
  Subtitles printed during the flight are skipped (GTA IV can't hide them
  for a frame). Put Betaflight's OSD disable on a switch (the radio wizard
  asks for one) to hide the OSD too, for recording. Mission markers and
  other things drawn in the world stay. In the pilot view (F) the HUD is back.
- **Move Niko here**: ends the flight with Niko standing where the drone is,
  facing the way it faced.

## Controls

| Key | Action |
|---|---|
| F7 | Launch the drone / end the flight back at Niko / from the F view, back to the drone |
| F8 | Settings overlay (radio setup, rates, OSD, camera, video feed, wind, pilot) |
| F | Back to Niko, the drone left flying (F7 returns to it) |
| C | Cycle cameras: FPV, chase, line of sight |
| R | Respawn the drone, repaired (in front of Niko after you used F) |
| Y | Move Niko here (also a button in F8 > Fly) |

F, C, R and Y are libfpv game controls: put them on your radio's switches or
buttons in F8 > Radio > "Set up game controls", or change their keys in
F8 > Controls. Letters are read by their position on the keyboard (a US
layout's), so on AZERTY the keyboard sticks stay under your left hand.

Without a radio, the keyboard works as a fallback. Enter arms, W/S is
throttle, A/D is yaw, and the arrow keys are pitch and roll. A gamepad works
too. In the F view the keyboard walks Niko, so it doesn't fly the drone then.

Fisheye strength and FOV are in F8 > Camera & sound. A strength of 0 turns
the lens off.

## Requirements

- GTA IV: The Complete Edition, version 1.2.0.59 (Steam). This is the only
  version that has been tested.
- [FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) (its ASI
  loader, `dinput8.dll`).
- Aru's C++ ScriptHook (`ScriptHook.dll`), for example from Tomasak's .NET
  ScriptHook pack.
- Windows 10 or 11. The game is 32-bit, so libfpv must be built for i686.

## Installing

**Players: follow the [install guide on the latest release](https://github.com/hammerill/gtaiv-fpv/releases/latest).**
It links everything you need and takes about five minutes.

Every push to `main` builds a new release (`.github/workflows/release.yml`)
with the latest libfpv, so `gtaiv-fpv.zip` is always current. The release
text comes from `docs/INSTALL.md`. To rebuild with a newer libfpv without a
mod change, run the Release workflow by hand from the Actions tab.

The ZIP holds these files, which go into the `GTAIV/` folder next to
`GTAIV.exe`. They are named after libfpv, as every libfpv game labels its
mod:

| File | What it is |
|---|---|
| `libfpv.asi` | Loader that the ASI loader picks up |
| `libfpv/libfpv_gtaiv.dll` | The mod itself (hot-reloaded by the loader) |
| `libfpv/libfpv.dll` | libfpv, i686 build |
| `libfpv/libfpv.LICENSE.txt`, `libfpv/LICENSE.txt` | libfpv's and the mod's license (GPL-3.0-or-later) |

Settings, radio mappings and Betaflight EEPROMs are in libfpv's shared
folder, `%APPDATA%\libfpv`, so your radio setup and tune follow you to the
other libfpv games. The first start copies an older version's
`GTAIV/fpv-data` there, unless another libfpv game already made the folder.
The log is `GTAIV/libfpv/libfpv_gtaiv.log`.

Updating from a version with `GTAIV.FPV.asi`: the new loader moves the old
files (`GTAIV.FPV.*`, the old `libfpv.dll`) into `GTAIV/libfpv/old` on its
first start. That one start still runs the old version; restart the game.

## Building

The build needs CMake 3.24 or newer and the i686 MinGW-w64 toolchain
(`i686-w64-mingw32-g++-posix`). It cross-compiles from Linux or WSL.

```sh
# libfpv, 32-bit (from the libfpv checkout)
cmake -S . -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-i686.cmake
cmake --build build/win32 -j

# this mod
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake
cmake --build build
./deploy.sh   # copies libfpv.asi into ../GTAIV, the core and libfpv.dll into ../GTAIV/libfpv
```

`LIBFPV_DIR` (default `~/Projects/libfpv`) and `LIBFPV_BUILD` point the
build at libfpv. Only the header is used at build time, because the core
loads `libfpv.dll` at runtime.

If you run `./deploy.sh` while the game is running, the new core reloads
within about a second. The `.asi` and `libfpv.dll` stay locked until the
game exits. The build writes the commit into libfpv's F8 > About tab
(`GTAIV_FPV_VERSION` names the version, CI sets `build-<n>`).

## How it works

- **ScriptHook from MinGW.** `ScriptHook.dll` exports only MSVC C++ symbols.
  `src/scripthook.cpp` finds them by their mangled names and builds the
  `ScriptThread` vtable by hand. Natives are called by joaat hash.
- **Loader and core.** The `.asi` owns the script thread, the log and the
  D3D hook. It forwards every tick and frame to the core DLL, and swaps in a
  new core when the file changes.
- **Camera.** A scripted camera follows libfpv's camera. The rotation is
  converted to the game's Euler order (heading, then roll, then pitch). A
  near plane is chosen from frustum probes so that the road stays visible
  under a low lens.
- **World collision.** libfpv's raycast callback goes to the engine's physics
  probe (found by reverse engineering 1.2.0.59). A hit is mapped back to its
  ped, vehicle or object through the entity pools.
- **Drawing.** ScriptHook's D3D9 hook calls `src/render.cpp` after the game
  finishes a frame. With the video feed on, the back buffer goes through
  libfpv's `docs/renderers/fpv_video_d3d9.h` (`src/video.cpp`, ps_3_0
  shaders compiled at runtime). Otherwise the fisheye is a 64x36 grid warped
  with libfpv's Brown-Conrady parameters, no shaders needed. The FOV is
  widened for the real picture aspect so that the edges still show the FOV
  you chose. The OSD and the overlay are drawn on top as textured triangles.
  The drone model is a small procedural quad projected through the frame's
  camera (`render.cpp`), hidden when a ray from the camera hits something
  first; it has no shadow.
- **Standing in.** With the stand-in on, Niko is moved to the drone at launch
  (hidden, frozen, no collision, spine on the drone, lifted clear while a car
  or ped is right next to it, his health topped up and every loss turned
  into broken props) and back home when the flight ends or on F
  (`src/drone.cpp`). The pose is `cellphone`/`cellphone_text` from the game,
  on Niko when he stays home.
- **Wind and damage.** `src/wind.cpp` maps `GET_CURRENT_WEATHER` to a mean
  wind (GTA IV has no wind direction: it blows towards the north-east),
  helicopters with a pilot to rotor sources and explosions (found with
  `IS_EXPLOSION_IN_SPHERE`) to blasts. `src/damage.cpp` does water
  (`GET_WATER_HEIGHT`, drag and per-prop damage), fire, explosions and rotors.
- **Audio.** A waveOut thread plays `fpv_audio_read`.

`MODLOG.md` is the development journal. It records addresses, decisions and
what was verified in game.

## Testing without hands

`GTAIV/libfpv/autotest.txt` turns on a scripted test. It moves Niko to a
road, launches the drone and flies a fixed stick sequence, logging
everything. Knobs: `car`/`ped` (crash target), `hover`, `still`, `cam`
(camera-axis test), `niko` (end with Move Niko here), `secs=`, `fov=`,
`near=`, `osd=`, `osdoff` (fly with the OSD disable switch on).
**Delete the file afterwards.** Otherwise every game start
runs the test.

Creating `GTAIV/libfpv/shot.txt` saves the next finished frame, with the
mod's own drawing, to `GTAIV/libfpv/shots/<time>.bmp`. This works when
window capture can't see the game.

## Credits

- [libfpv](https://github.com/hammerill/libfpv): the drone, Betaflight
  (through SimITL), OSD, overlay and sound.
- Aru: the GTA IV C++ ScriptHook. ThirteenAG: FusionFix and the ASI loader.
- Written with Claude (Anthropic).

## License

GPL-3.0-or-later, the same as libfpv (which embeds Betaflight). See
[LICENSE](LICENSE) for the full text.
