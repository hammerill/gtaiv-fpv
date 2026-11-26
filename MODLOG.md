# GTA IV FPV drone — MODLOG

## Goal
FPV drone mode in GTA IV (Complete Edition, Steam 12210) powered by libfpv (~/Projects/libfpv).
Done = fly the drone in game with a radio: FPV/chase/LOS cams, OSD, motor sound, F8 overlay,
world collision, impacts that ragdoll/damage peds and vehicles.

## Environment (recon 2026-10-13)
- Game: /mnt/d/SteamLibrary/steamapps/common/Grand Theft Auto IV/GTAIV/GTAIV.exe
  - 1.2.0.59, PE32 (i386), D3D9, md5 1a47b45f9e254a8dec8a347ece43c8ac
  - No anti-cheat, no loaders installed (vanilla). Launches via Steam -> Rockstar Games Launcher.
- Profiles/saves: C:/Users/Hammerill/Documents/Rockstar Games/GTA IV/Profiles/FA15D3B0
- Settings: C:/Users/Hammerill/AppData/Local/Rockstar Games/GTA IV/Settings
- libfpv: ~/Projects/libfpv (C API, include/libfpv.h). Only an x64 MinGW toolchain exists.
- Toolchains present: i686-w64-mingw32-{gcc,g++-posix}, x86_64-w64-mingw32-gcc.
- KB: no prior field notes for GTA IV or FPV.

## Decisions (agreed with user)
1. 32-bit bridge: add an i686 MinGW toolchain to libfpv and load libfpv in-process.
2. Loader stack: FusionFix (ASI loader, dinput8.dll) + Tomasak's .Net ScriptHook pack (user-installed;
   ships Aru's C++ ScriptHook.dll 0.5.1, which auto-detects 1.2.0.59). Mod is a native .asi.
3. Drone body: a small vanilla prop; collision via the game's world probe through
   fpv_world_set_raycast_callback.
4. Extras in v1: crash effects on peds/vehicles (ragdoll + damage). No fisheye, no kamikaze, no clip.
5. Repo: Grand Theft Auto IV/gtaiv-fpv (this folder). Deploy copies build output into GTAIV/.
6. Toggle: hotkey (default F7, configurable .ini) launches drone from Niko; Niko frozen; same key returns.
7. Input: user's radio via libfpv SDL. Automated testing allowed (check user idle first).

## Plan
1. Backups: game folder file list + profiles/settings (um backup).
2. libfpv i686 build (toolchain file, fix 64-bit assumptions), run tests under wine if possible.
3. Install FusionFix + ScriptHook (current releases), verify a hello .asi loads.
4. Vertical slice: hotkey -> scripted camera follows libfpv pose, player frozen.
5. Raycast callback via game world probe (RE needed), impacts -> ragdoll/damage.
6. Prop body, OSD + overlay renderer (D3D9), audio (libfpv -> game/own output).

## Log
- 2026-10-13: Backups: ~/.universal-modder/backups/gtaiv-profiles/20261013-185132.zip,
  gtaiv-settings/20261013-185133.zip. Vanilla GTAIV/ root files hashed in vanilla-root-files.md5
  (restore = delete anything in GTAIV/ not listed there, e.g. dinput8.dll, *.asi, plugins/).
- 2026-10-13: libfpv i686 build works. Branch `win32-i686` in ~/Projects/libfpv (commit cd10d3a):
  cmake/toolchains/mingw-w64-i686.cmake, build dir build/win32. Needs `-msse2 -mfpmath=sse`
  (x87 default made wall_crash tunnel through the wall). 20 tests / 321 checks pass under
  Windows via WSL interop (copy libfpv.dll next to tests/fpv_tests.exe first, else exit 53).
  Note: build/win (x64) tests dir holds a stale libfpv.dll -> 30 bogus failures; unrelated.
  Next: install FusionFix + Zolika ScriptHook, hello-world .asi.
- 2026-10-13: User installed FusionFix + Tomasak's ScriptHook pack. New files in GTAIV/:
  dinput8.dll (ASI loader), d3d9.dll + vulkan.dll + d3d9.cfg (DXVK from FusionFix), plugins/GTAIV.EFLC.FusionFix.{asi,ini,cfg},
  update/, ScriptHook.dll, ScriptHookDotNet.asi, aCompleteEditionHook.asi, scripts/. scripthook.log (19:15 run, user's
  test) shows "Using game version Dynamic (1.2.0.59)" and ScriptHookDotNet initialised -> the hook works on this exe.
- **User launches with GTAIV/PlayGTAIV.exe, NOT Steam** (a Steam update could wipe the mod). Do the same.
- ScriptHook API: Aru's SDK headers cloned to ~/gtaiv-ref/gtaiv-scripthook-sdk (github violetfrost/gtaiv-scripthook-sdk;
  reference only, Aru's license, don't commit). ScriptHook.dll exports MSVC C++ only, so src/scripthook.{h,cpp} is a
  MinGW shim: GetProcAddress on mangled names, ScriptThread built by hand (ctor ??0ScriptThread@@QAE@XZ via thiscall,
  copy its vtable incl. RTTI slot [-1], override [0] RunTick and [4] scalar deleting dtor; vtable order verified from
  ??_7ScriptThread@@6B@: RunTick, RunScript, OnStart, OnKill, dtor). Register with
  ?RegisterThread@ScriptHookManager@@SAXPAVScriptThread@@@Z (cdecl). Natives via
  ?SpeedyInvoke@NativeInvoke@@CAXPAVNativeContext@@I@Z (cdecl, ctx layout in scripthook.h).
  Native hash = joaat(lowercase name) (verified: get_player_id = 0x1cc648ea matches SDK). Macro N("NAME").
  ScriptHook also exports ID3DHookService / IKeyboardHookService (vtables dumped with
  scratch script; not used yet; could be useful for OSD drawing).
- Hello-world .asi built (build: `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake && cmake --build build`,
  deploy: ./deploy.sh -> GTAIV/GTAIV.FPV.asi). Writes GTAIV/GTAIV.FPV.log ("attached", "ScriptThread registered",
  "first script tick"); F7 logs Niko's coords and prints a help text. **NOT YET VERIFIED IN GAME.**
- Added GTAIV/commandline.txt (-windowed -width 1280 -height 720) for stable screenshots. Undo: delete it.
- Testing attempts (2026-10-13 ~19:25):
  - `um win launch <exe>` timed out (cmd.exe started from a /mnt/d cwd); launch from /mnt/c instead:
    `cd /mnt/c && cmd.exe /c start "" "D:\...\GTAIV\PlayGTAIV.exe"`. `um win setup` done (ffmpeg gfxcapture, h264_qsv).
  - Game opened the CE episode picker (GTA IV / EFLC), still 1920x1080 fullscreen (commandline.txt not applied
    at that stage?). Screenshots come out 1920x1080 regardless of --scale. Window rect 0 0 1920 1080.
  - WinDrive `click` did nothing (cursor didn't move); `scanmode on` + `key 0x0D` sent but no visible progress.
  - **No .asi loaded at the picker**: scripthook.log / ScriptHookDotNet.log not rewritten, no GTAIV.FPV.log.
    User stopped the game twice after ~2 min of nothing. Unknown whether Enter started loading.
  - User asked me to stop and continue in a later session.

- 2026-10-13 22:27: **Hello-world VERIFIED in game** (user launched PlayGTAIV.exe, loaded save, F7): GTAIV.FPV.log shows
  attached -> ScriptThread registered -> first script tick -> "F7: player 0 ped 2 at 601.42 1409.84 17.48". Root *.asi load fine.
- Vertical slice written (src/drone.{h,cpp}), built, deployed, NOT yet tested in game:
  - libfpv.dll loaded lazily (LoadLibrary from GTAIV/) on first F7; data_dir GTAIV/fpv-data; log -> GTAIV.FPV.log.
  - coords: right +X, up +Z, forward +Y, 1 unit = 1 m (GTA IV world: X east, Y north, Z up).
  - F7: drone spawns 2 m in front of Niko on GET_GROUND_Z, yaw = Niko heading; SET_PLAYER_CONTROL off;
    CREATE_CAM 14 + ACTIVATE_SCRIPTED_CAMS; each tick SET_CAM_POS/ROT/FOV from frame.camera.
    Euler: heading = atan2(-fx, fy), pitch = asin(fz), roll = atan2(-right.z, up.z) — **roll sign unverified**.
  - Keys (only when game window focused): libfpv keyboard fallback W/S/A/D/arrows/Enter/T/B; C cycles FPV/chase/LOS;
    R resets to spawn + repair; F7 back to Niko (destroys cam, restores control, deletes drone).
  - Collision: raycast callback answers only downward rays (dir.z < -0.5) via GET_GROUND_Z_FOR_3D_COORD. No walls yet.
  - Log line every 2 s: drone pos, flags, altitude, throttle, arming-disable text.
  - deploy.sh now also copies ~/Projects/libfpv/build/win32/libfpv.dll to GTAIV/.

- 2026-10-13: **Slice VERIFIED in game** by the user: flies, camera follows, roll sign correct. Radio "Arduino RaspberryPi
  Pico" detected as radio (log says "needs setup" but the user flies it fine). frame.altitude stays -1 (ground ray not
  answered for its query? check later).
- World raycast research: no raycast native in IV. IV-SDK .NET (~/gtaiv-ref/IV-SDK-DotNet, reference only) has
  CWorld::ProcessLineOfSight(CVector* from, CVector* to, uint32* unk=0, tLineOfSightResults* res, flags=142, 1, 0,
  seeThrough=2, 4), cdecl, returns bool; results 0x54 bytes: +0 phInstGta*, +0x10 end position, +0x20 / +0x30 vectors
  (likely normal), flags 2 static, 4 buildings, 8 vehicles, 32/64 peds, 128 objects. Addresses only for 1.0.7.0 (0x596D80)
  and 1.0.8.0 (0x52A400), not 1.2.0.59. Native hashes are not plain in GTAIV.exe (registration obfuscated), so
  added src/recon.cpp: on first tick, logs handler addresses via ScriptHook's Game::GetNativeAddressFromHash for
  GET_GROUND_Z_FOR_3D_COORD etc. and dumps the in-memory image to GTAIV/fpv-data/recon/gtaiv-image.bin (once).
  Plan: disassemble those handlers offline (capstone via `uv run`, script in scratchpad) to find the line test.

- 2026-10-13 22:43: recon ran (image base 0x700000, dump in GTAIV/fpv-data/recon, local only). Handlers (RVA):
  GET_GROUND_Z_FOR_3D_COORD 0x7944e0, GET_CHAR_HEIGHT_ABOVE_GROUND 0x79ee70, HAS_CHAR_SPOTTED_CHAR 0x79f6d0,
  IS_POINT_OBSCURED_BY_A_MISSION_ENTITY 0x7949f0, GET_WATER_HEIGHT_NO_WAVES 0x7de3a0.
  Chain (VA @0x700000): native 0xe944e0 -> 0xe962d0 -> FindGroundZ 0xd523f0(x,y,z,bool*,0,4) -> vertical probe
  0xd54510(CVector* start, float endZ, phInst* exclude, Intersection* out, typeFlags, ?) -> **general probe 0xa38880**,
  thiscall on *(void**)0x15b9c78 (phLevel), ret 0x20: (Segment* {vec4 a, vec4 b}, Intersection* out, phInst* exclude,
  typeFlags, -1, 7, 1, 0) -> nonzero on hit. Intersection 0x54: +0 phInst*, +0x10 position, +0x20/+0x30 vectors
  (normal candidates, logged for the first 10 hits), +0x40 t, +0x4c part (0xffff), +0x50 material byte.
  Disassembler: scratchpad gtadis.py (`uv run -q gtadis.py <dump> <va>`; don't name it dis.py).
- src/world.cpp: libfpv raycast callback through that probe, types 2|4|8|64|128. Signature-checked (prologue bytes +
  `mov ecx,[level]` at vertical probe +0x60); falls back to ground-only GET_GROUND_Z on mismatch. NOT yet tested.

- 2026-10-13 22:50-22:57: **World collision VERIFIED (self-tested, user asked me to test myself).**
  Probe sweep indoors (Niko's Broker apartment): walls 3-9 m, normals clean; nA == nB always (use either). Outdoors
  (road 585,1410,10): buildings ~12 m each side, open along the road. Drone: climbed, hit the apartment ceiling (n 0,0,-1);
  run 2 cruised into a building wall at x=597 (n -1,0,0), prop strike, crash, fell and landed on the street (n up).
  Rays include peds (type 64): a down ray from Niko's head hits Niko himself (inst differs). Fine for now.
- Radio: input source was KEYBOARD ("needs setup", overlay not drawn yet). Copied the user's sm64ex mapping
  (C:/Users/Hammerill/AppData/Roaming/sm64ex/libfpv-data/radio_mappings.json) into GTAIV/fpv-data -> source RADIO.
- **Self-test workflow** (src/autotest.cpp): touch GTAIV/fpv-data/autotest.txt -> 10 s later (also on game start if the
  file exists) it moves Niko to the nearest road (GET_CLOSEST_CAR_NODE + LOAD_SCENE), logs an 8-way + down probe
  sweep, launches the drone and flies via fpv_input_set_channels: arm at 2 s, altitude hold (+4 m target; hover is
  about thr -0.7 in ANGLE), pitch 0.25 from 7 s, stops at 20 s. **Delete autotest.txt before handing back to the user**
  (otherwise every game start teleports Niko). Loop: kill exact GTAIV pid (`um win ps`), ./deploy.sh, relaunch from /mnt/c
  with `timeout 10 cmd.exe /c start "" "D:\...\GTAIV\PlayGTAIV.exe"` (no episode picker appeared, loads straight
  into the save ~18 s), wait for "autotest: flying" in the log, `um win shot --exe GTAIV.exe x.png --scale 0.33`.
  Check `um win drive --proc explorer idle` first.
- Throttle note: 5" PropEater hovers near thr -0.7 (15% stick); thr 0.1 shot it to 700 m in 18 s.

- 2026-10-13 23:00-23:22: **Impacts on vehicles and peds VERIFIED (self-tested).**
  - Entity pools (1.2.0.59, VA @0x700000, `mov ecx,[pool]` sites signature-checked in world.cpp): peds *(0x1bb6f1c)
    (site 0xeb37b9 in GET_PLAYER_CHAR's helper 0xeb37b0), vehicles *(0x15e22a4) (0xea653c), objects *(0x1932c60)
    (0xd04664). Pool {objects, flags, count, entry_size}; handle = index<<8 | flags[index] (game fn 0x7b7020,
    handle->ptr 0x799d20, both thiscall on the pool). PlayerInfo+0x598 = CPed*, CPed+0xb30 = vehicle.
    phInst+0xC = CEntity* (IV-SDK layout holds). Verified: Niko -> ped handle 2 (= GET_PLAYER_CHAR).
  - fpv object_id = kind<<28 | handle (1 ped, 2 vehicle, 3 object, 4 building/other).
  - effects.cpp: ped >= 4 m/s -> SWITCH_PED_TO_RAGDOLL(ped,1000,3000,0,1,1,0) + DAMAGE_CHAR(speed*2) + APPLY_FORCE_TO_PED;
    car >= 5 m/s -> SET_CAR_HEALTH(-speed*3) + APPLY_FORCE_TO_CAR; objects -> APPLY_FORCE_TO_OBJECT. Cooldown 0.5 s per
    entity, started only when an effect applied (a 2 m/s graze first used to swallow the real hit). Player skipped.
  - Results: car 1000 -> 979 at 7.1 m/s; ped ragdoll=1, 200 -> 185 (178 after the fall) at 7.8 m/s.
  - Native hashes in GTAIV.exe registration are transformed (not joaat, not xor/add const); use ScriptHook's
    Game::GetNativeAddressFromHash at runtime (recon.cpp logs handlers each start).
- **Hot reload**: GTAIV.FPV.asi is now a loader (src/loader.cpp) owning the ScriptHook thread, the log ("w") and
  libfpv's log callback (export gtaiv_fpv_libfpv_log, so libfpv never calls into an unloaded core). It polls
  GTAIV.FPV.core.dll's timestamp every 30 ticks, calls the core's gtaiv_fpv_core_unload (stops drone + autotest),
  FreeLibrary, copies to GTAIV.FPV.core.live.dll and loads that. ./deploy.sh while the game runs = reload in ~1 s
  (the asi and libfpv.dll stay locked; deploy says "kept the running one").
- Launcher gotcha: killing GTAIV.exe leaves PlayGTAIV.exe + Launcher.exe thinking the game runs; the next launch only
  shows Rockstar's "A game is currently running" dialog. Kill those two by PID (tasklist) before relaunching.
  With hot reload, relaunches are rarely needed.
- Test script: scratchpad run_test.sh <car|ped> <shot-prefix> (hot reload if running, else cold start; reads only new
  log lines; 4 screenshots). Autotest now faces north on the Broker road (585,1410) with the target 16 m ahead,
  holds 0.7 m, pitch 0.5.

- 2026-10-13 23:28: **OSD + F8 overlay VERIFIED on screen (self-tested).** Loader registers a hand-built
  ID3DDeviceHook with ScriptHook's "D3DHook" service (RequestService export, AddDeviceHook = service vtable [5];
  hook vtable: OnCreateDevice, OnResetDevice, OnLostDevice, OnDestroyDevice, OnRender, all thiscall) on its first
  tick. OnRender (render thread) -> core gtaiv_fpv_core_render under a shared SRWLOCK; reload takes it exclusively and
  first asks the render thread to free the core's textures (event, 500 ms timeout). src/render.cpp: fixed-function
  XYZRHW|DIFFUSE|TEX1, DrawIndexedPrimitiveUP with INDEX32, managed A8R8G8B8 font textures (RGBA->BGRA, re-upload on
  revision), per-frame D3DSBT_ALL state block + RT/DS save (ImGui DX9 backend style). OSD (analog style) from a
  mutex-guarded frame copy, only in the FPV view. Overlay mouse from GetCursorPos on the device's focus window; libfpv
  draws its own cursor and has keyboard nav. Screenshots showed Betaflight OSD (25.7 V, 4.28 V/cell, timer, ANGL,
  amps) and the overlay's Fly tab (arming-disable reasons, motor/prop state).
- libfpv code moved to src/fpvapi.{h,cpp} (FPV_FUNCTIONS X-macro). F8 loads libfpv on foot too; while the overlay
  wants input on foot, SET_PLAYER_CONTROL off. Forwarded keys now also F8, Esc, Tab, Backspace, Space.
- Log fix: loader truncates once, then loader and core both append (loader "w" used to overwrite core lines).
- **F8 is stuck "down" system-wide on the user's PC** (GetAsyncKeyState 0x77 & 0x8000 from a separate process,
  scratchpad keys.exe -> C:/Users/Hammerill/AppData/Local/Temp/fpv-keys.exe). It opened the overlay at startup.
  Fix: edge detectors treat a key's first reading as a baseline (core.cpp/drone.cpp). Tell the user; pressing F8
  physically should clear it.

- 2026-10-13 23:35: Probes ignore the player (VERIFIED: sweep down ray from Niko's head now hits the road at 2.01 m).
  CEntity+0x38 = phInst (back-link checked), passed as the probe's `exclude`; that alone wasn't enough (peds have more
  instances, e.g. the ragdoll frag), so hits on the ignored entity restart the probe 25 cm further (max 8 times).
  Ped handle -> entity via the pool (index = handle>>8, flags must match). SET_CAM_NEAR_CLIP(cam, 0.05).
  autotest.txt deleted after testing.

- 2026-10-13 23:55: User reported flickering textures in drone mode + no drone sound.
  - Flicker: blamed on SET_CAM_NEAR_CLIP 0.05 (depth precision -> z-fighting); now kNearClip 0.2 in drone.cpp.
    Self-test screenshots at 0.2 look clean; an A/B at 0.05 was not finished. **User testing.** If it still
    flickers, next suspects: world streaming centred on Niko, not the camera; the D3D state block in render.cpp.
  - Audio was never implemented. src/audio.cpp: waveOut thread (44.1 kHz stereo s16, 4x512 frames) pulling
    fpv_audio_read; started on launch, stopped (joined) in drone stop before the drone is deleted. Log shows
    "audio: started", no waveOut error. Not heard yet. Scratchpad rec_test.sh can record game-only audio.

- 2026-10-14 00:05-00:40: User: FPV-only flicker (less at near 0.2), mirrored view on flips, see-through ground when
  landed; asked for an F8 option to turn off FOV/fisheye "post-effects". All fixed and self-tested:
  - **Flips**: cam test (autotest.txt "cam", GET_VIEWPORT_POSITION_OF_COORD on axes predicted by all 6 Euler orders)
    showed the game builds cameras as Rz(heading) Ry(roll) Rx(pitch), not Rz Rx Ry. drone::basis_to_euler now
    uses roll = asin(-r.z), pitch = atan2(f.z, u.z), heading = atan2(r.y, r.x) (knife-edge fallback). Pass 2 of the
    cam test VERIFIED all 12 cases (inverted, knife edge, past vertical) land forward/up/right correctly.
  - **Flicker**: measured with scratchpad tools/flicker.py (A-B-A pixel toggles in `um win record` clips). FOV and near
    clip made no difference; the OSD pass did: D3DSBT_ALL state block + SetRenderTarget/SetDepthStencilSurface(null)
    swap, even without drawing anything, made the next frame show stale regions and exposure jumps. render.cpp now
    draws on the bound back buffer (it always is: ~150 calls/s, back buffer bound) and saves/restores only the states it
    touches (SavedState). Parked-with-OSD toggles 1-6% (peaks 86%) -> 0.000; hover 0.000.
  - **See-through ground**: near plane (0.2, ±30 cm tall at this FOV) cut the road under a 7 cm-high lens. Now
    near_clip_for() probes 9 frustum rays (centre/edges/corners, aspect up to 2.4) and sets near = 0.8 x closest depth,
    clamped 0.02..0.2. Parked: near 0.079, road rendered to the bottom edge.
  - **FOV/fisheye**: the mod never drew libfpv's lens pass, but libfpv widened the FOV for it (100 -> ~112 vertical).
    New libfpv flag FPV_INIT_NO_LENS_DISTORTION (libfpv working tree, branch win32-i686, NOT committed; 20 tests pass):
    no widening, k1=k2=0, F8 > Camera shows "Fisheye lens: not drawn by this game". The FOV slider there still works.
    Possible later: real fisheye pass in D3D9 (StretchRect + pixel shader).
  - Audio recording check: flying clip RMS 0.043 vs parked 0.014 -> motor sound reaches the game's audio output.
  - autotest.txt knobs: car|ped|cam, still, hover, fov=, near=, osd=. Careful: a hot reload re-triggers the
    existing autotest.txt, so recordings lagged one run until scratchpad at.sh stopped deploying between runs.
  - **User wants the game closed after testing** (GTAIV + PlayGTAIV + Launcher by PID).

- 2026-10-14 00:50-00:58: User asked: commit, fisheye, "Move Niko here", hide HUD, README. All done and self-tested.
  - Committed the earlier work (mod f288204, libfpv a7d8960 on main; win32-i686 was merged into main by then).
  - **Fisheye VERIFIED**: render.cpp StretchRects the back buffer into a D3DPOOL_DEFAULT render-target texture
    (released on device lost) and draws it back through a 64x36 grid whose texcoords follow libfpv's Brown-Conrady
    formula (no shader compiler needed). Only in the FPV view, before the OSD/overlay. FPV_INIT_NO_LENS_DISTORTION
    is no longer passed. drone.cpp lens_fov() redoes libfpv's FOV widening for the real back-buffer aspect
    (render::aspect()) instead of the 4:3 aspect_hint: FOV 119.2 at 16:9. Single application per frame: the
    screenshot shows a moderate barrel warp (lamp poles bowed), not a doubled one.
    The launch help text (PRINT_STRING) was warped off screen by the lens; user chose to remove it (no libfpv
    message API, no own text drawing).
  - **HUD hidden VERIFIED**: HIDE_HUD_AND_RADAR_THIS_FRAME every drone tick (all views); no state to restore.
  - **Move Niko here VERIFIED** (button + key): libfpv got fpv_overlay_set_action(id, label) /
    fpv_overlay_poll_action() (libfpv 41c30f1, test added, 20 tests pass win32 + linux). Button "Move Niko here (N)"
    in F8 > Fly while flying; N key too. drone::move_niko_here(): probe down from the drone, stop(), LOAD_SCENE,
    SET_CHAR_COORDINATES (WARP_CHAR_FROM_CAR_TO_COORD if in a car) at ground + 1, heading = drone's,
    SET_CAM_BEHIND_PED. Clicked via WinDrive (`click 639 500` at 1920x1080) and `key 0x4E`; Niko moved 13 m.
  - New libfpv exports mean a libfpv.dll change needs a game restart (hot reload keeps the old dll).
  - autotest knobs: `niko` (end with Move Niko here), `secs=<flight seconds>`. WinDrive keys take hex VKs (`key 0x77`).
  - README.md + docs/fpv-view.jpg added.

- 2026-10-14 01:20-01:50: User asked: world (terrain, buildings, LODs, NPCs, cars) must stream around the drone, not Niko.
  - FindPlayerCentreOfWorld is inlined all over 1.2.0.59 (FindPlayerInfo = 0x7757c0, player-in-focus index *(0x1336f14),
    player infos 0x14a8808[32], GetPlayerInfo(id) = 0xc3fbe0), so no single hook. Instead **Niko is a streaming anchor**
    (drone.cpp Anchor): drone > 80 m from spawn -> Niko frozen, invisible, no collision, invincible, everyone/police
    ignore player, warped out of his car (handle kept), re-placed at the drone every 10 m. Back within 40 m of home,
    F7, or R -> LOAD_SCENE home, back into his car if DOES_VEHICLE_EXIST, else his spot. Move Niko here keeps him there.
  - User test 1: long flights worked, but the drone often spawned under the ground. Cause: the first frames after
    fpv_drone_create are stale (origin, or the previous drone; libfpv's sim clock is global, never reset per drone),
    so the anchor teleported Niko to 0,0,0 for 0.5 s and the spawn's collision unloaded. Fix: drone ticks ignore frames
    until sim_time moved 50 ms past its first reading (also stops a camera jump). Possible libfpv fix later (ask first).
  - User test 1: no OSD after Load Game. scripthook.log: loading kills and restarts all script threads (and, presumably,
    its D3DHook service, dropping our device hook). scripthook.cpp now overrides ScriptThread OnKill (vtable [3]) to
    count kills; loader re-adds the D3D hook if no OnRender arrives within 60 ticks after a restart.
    **Both fixes NOT yet verified; the user is testing.** Unhandled: loading a save while flying.
  - autotest knobs: `far` (climb 35 m, cruise from 12 s), `incar` (Niko in the target car at launch).

- 2026-10-15 02:50: Synced with libfpv 81afd28 (from 41c30f1: GPS, OSD foreground layer, air mode AUX5, OSD disable AUX6).
  - fpv_frame.channels is now FPV_CHANNEL_COUNT (10): rebuilt; inject_channels/autotest send 10 channels.
  - fpv_gps_set_home bound (new export: an old libfpv.dll now fails to load, restart the game after deploy). Home = Niko:
    set at launch, then every tick to his position, or to g_anchor.home while he rides along as the streaming anchor.
  - FPV_DRONE_OSD_DISABLED (libfpv INTEGRATION.md "OSD disable switch"): every flight tick with the flag,
    HIDE_HELP_TEXT_THIS_FRAME + CLEAR_PRINTS (subtitles have no per-frame hide, so ones printed meanwhile are lost).
    HUD/radar were already hidden; world markers stay (README says so). Mod draws nothing else of its own in flight.
  - autotest knob `osdoff` (AUX6 high). **VERIFIED** cold start: libfpv ran "enabling Betaflight's GPS feature" and
    "upgrading Betaflight configuration for new mode switches" on the existing EEPROM, flags 0x241/0x260 (0x200 set),
    hover stable, no native errors. Help/subtitle hiding and the GPS home arrow NOT yet seen on screen by the user.

- 2026-10-15 15:00-15:26: **Kamikaze support** (libfpv 31a7bc8, "surprise build flag", -DLIBFPV_KAMIKAZE=ON). User's choices:
  ending toggle as an F8 > Fly button (saved in fpv-data/gtaiv-fpv.ini, `after_blast=signal_lost|cutscene`), grenade
  explosion, release ZIP keeps the normal libfpv.
  - Mod compiles with LIBFPV_KAMIKAZE always (FPV_CHANNEL_COUNT 12, bigger fpv_frame); fpv_kamikaze_poll_detonation /
    fpv_kamikaze_get_state are optional GetProcAddress (fpvapi::kamikaze()). A normal dll copies min(struct_size) and
    clamps channels, so it just works. Local kamikaze libfpv: ~/Projects/libfpv/build/win32-kamikaze (tests on, examples
    off); deploy with `LIBFPV_DLL=.../build/win32-kamikaze/libfpv.dll ./deploy.sh`. **GTAIV/libfpv.dll is now the
    kamikaze build.**
  - src/kamikaze.cpp: ADD_EXPLOSION(x,y,z, 0 grenade, 5.0, sound, visible, shake 1.0) at fpv_detonation.position; the
    ending setting + button (action id 2). drone.cpp: detonations drained at launch, polled every tick; ending state:
    Signal lost = FPV view forced, audio::set_static (white noise), render draws 320x180 point-filtered static + libfpv's
    OSD (its font has a dark outline, no extra backing needed), 4 s; Cutscene = g_cam moved 14 m back
    (against the drone's horizontal velocity) and 5 m up, ray-checked over 8 turns, looking at the blast +1 m, FOV 50,
    5 s. Then stop() (Niko home). During the ending C/R/N and Move Niko are ignored. Niko dead -> stop(false).
    K/X forwarded for the keyboard fallback.
  - **VERIFIED in game (self-tested):** normal libfpv -> "no warhead", flight + car hit as before. Kamikaze dll: impact
    into the car -> "warhead detonated (impact)", explosion, car health 0, static + BOOM + SIGNAL LOST readable, back to
    Niko after 4 s; cutscene: outside shot of fireball, then burning wreck, back to Niko after 5 s; explode switch air
    burst at 2.6 m from Niko: he survived, nearby car scorched. F8 "After the blast" button NOT clicked in game (no
    input from session 0); the hiss NOT listened to.
  - **Testing gotcha (new):** this session's WSL starts Windows processes in session 0 (Services): the game launches
    without a window, `um win ps` lists nothing, gfxcapture fails. Workaround the user approved: temporary scheduled
    task `schtasks /Create /TN gtaiv-fpv-test /TR "<PlayGTAIV.exe>" /SC ONCE /ST 23:59 /IT` + `schtasks /Run`, deleted
    afterwards; screenshots via the mod itself (touch GTAIV/fpv-data/shot.txt -> fpv-data/shots/*.bmp). Stale
    PlayGTAIV/Launcher from the previous night had to be killed first.

- 2026-10-15 15:45: libfpv 6d0d258 draws "SIGNAL LOST" itself (OSD row 7, instead of BOOM, kept until repair, even
  with the OSD disable switch on). The mod no longer writes its own text or dims the static behind it (user: the font
  already has a dark background).
  **VERIFIED in game**: one SIGNAL LOST, readable, through the whole 4 s.

- 2026-10-24: drone letter keys are polled by scan code through the game window's layout (AZERTY had W/A/S/D
  scattered). Move Niko here moved from N to Y, the key libfpv's INTEGRATION.md recommends (sm64ex-fpv uses it too).
  NOT compiled or tested in game (written on the Mac, no MinGW).

- 2026-10-24: **Generalized kamikaze ending** (libfpv's KAMIKAZE_ENDING contract: `fpv_kamikaze_set_ending_support`,
  `fpv_kamikaze_config.ending_view/ending_then` after `ending_duration`). The F8 > Fly "After the blast" button and
  fpv-data/gtaiv-fpv.ini are gone (init clears action id 2 left by an older core); the choice lives in libfpv's
  F8 > Kamikaze tab. kamikaze::init calls set_ending_support(all views, all thens, SIGNAL_LOST, RESPAWN) when the dll
  has it (optional GetProcAddress; older kamikaze dlls -> signal lost / 3 s / respawn). fpv_kamikaze_get_config is
  now part of the required kamikaze set. At the detonation drone.cpp reads view/duration/then: signal lost = static +
  libfpv's SIGNAL LOST, HUD hidden + HIDE_HELP_TEXT/CLEAR_PRINTS every tick; cutscene = the old blast camera.
  Timer = clamped script-tick dt, not counted while IS_PAUSE_MENU_ACTIVE. Then RESPAWN = repair + reset at the spawn
  (Niko's anchor home first), RECALL = stop() like F7, NOTHING = hold. Duration 0 skips the view (with NOTHING the view
  is held anyway, else the frozen wreck would just sit there). During the view R / F7 / Y (and the F8 Move Niko button)
  act at once; C is ignored. R now does repair before reset (INTEGRATION.md order). Autotest knobs
  `respawn`/`recall`/`hold`, `dur=<s>`. Pilot visibility untouched (out of scope for IV).
  Built against libfpv with the new header; NOT tested in game.

- 2026-11-25: **Synced with libfpv 1.1.3 (HEAD 936dad3) and its current INTEGRATION.md**, ~95 libfpv commits since
  the last sync (ff2479f). Built (no warnings) and deployed; NOT run in game (the user tests).
  - Naming ("Label the mod libfpv"): `GTAIV/libfpv.asi` + `GTAIV/libfpv/{libfpv_gtaiv.dll, libfpv.dll, log, autotest.txt,
    shot.txt, shots/}` (rdr2-fpv's layout). Exports `libfpv_gtaiv_*`. The loader moves `GTAIV.FPV.*`, the root
    `libfpv.dll`/LICENSE into `libfpv/old` on its first tick and stays idle that session if `GTAIV.FPV.asi` is loaded.
  - Data: `fpv_shared_data_dir()` (%APPDATA%\libfpv), copying `GTAIV/fpv-data` only if the shared folder doesn't exist
    yet (on the dev PC it already exists, so the GTA IV settings were NOT copied: the other game's setup wins).
  - About tab: host_name/host_build/host_url; `cmake/build_info.cmake` writes the commit on every build.
  - All modern functions are now required in fpvapi (shipped together); kamikaze stays optional.
  - Video feed: libfpv's `docs/renderers/fpv_video_d3d9.h` via `src/video.cpp` (copied from gtasa-fpv), shaders
    compiled on a thread; `FPV_INIT_VIDEO_FEED` only when d3dcompiler_47.dll loads. Falls back to the grid lens.
    lens_fov uses the feed's picture aspect with the resolution effect. Own static only when the noise is off.
  - Game controls (`fpv_controls_set`): gtaiv.camera C, gtaiv.respawn R, gtaiv.pilot_view F, gtaiv.move_niko Y,
    INSTANT. All letters/digits now go to `fpv_input_key` by scan code; typed text via ToUnicodeEx ->
    `fpv_overlay_text`. In the pilot view keys reach libfpv only while the overlay is open (+F8).
  - Pilot view (F): scripted cam off (`ACTIVATE_SCRIPTED_CAMS false`), `SET_CAM_BEHIND_PED`, Niko's control back
    once F (scan 0x21) is up, HUD shown, `fpv_set_view(PILOT)`, listener = GET_GAME_CAM pos/rot -> quaternion
    (Rz Ry Rx inverse of basis_to_euler). F7 from there returns to the drone. Respawn after F: in front of Niko now.
  - Stand-in replaces the 80/40 m anchor: `fpv_pilot_set_support(STAND_IN|POSE, 20, 10)`, distances from
    `fpv_pilot_get_config`. Niko frozen/invisible/no collision at the drone, NOT ignored by AI any more,
    `SET_CHAR_COORDINATES_NO_OFFSET` with BONE_SPINE1 (0x4B3) on the drone (self-correcting offset), health held at
    1000 and every loss -> `fpv_drone_damage_props`. A copy (`CREATE_CHAR` type 0 with his model and the 11
    component variations, invincible, never targetted) stands at home, or at the wheel of his car (car frozen past
    100 m). Refused (Niko stays home) as a passenger or in a car on a mission. Respawn waits up to 10 ticks for the
    frame to show the spawn before the distances count.
  - Pose: `cellphone`/`cellphone_text` (TASK_PLAY_ANIM loop) on Niko or the copy with FPV_DRONE_RADIO_ACTIVE, not in a
    car, not in the pilot view; hand-over copies GET/SET_CHAR_ANIM_CURRENT_TIME. Anim names were found by decrypting
    pc/anim/anim.img's TOC (AES key located in GTAIV.exe by its SHA-1) and hashing candidates against
    cellphone.wad: cellphone_in/out/talk/text exist; amb@tourist_camera / amb@newspaper_* names unknown.
  - Wind: GET_CURRENT_WEATHER -> 1.5..11 m/s towards the NE (IV has no wind direction), helis with a driver from the
    vehicle pool -> rotor sources (mass 2500 kg, radius from GET_MODEL_DIMENSIONS), explosions within 60 m located
    with IS_EXPLOSION_IN_SPHERE probes -> `fpv_wind_add_blast`; the warhead's own blast directly. ENCLOSED while
    Niko stands in inside an interior.
  - Damage: GET_WATER_HEIGHT(x,y,z,&h) per hub (1/s) + drag push (k 6/s) on the centre; fires within 1.5 m
    (0.5/s); explosion within 4 m (1, once); a piloted heli's rotor disc (1).
  - Raycast: `fpv_world_set_raycast_callback_ex` with FPV_RAYCAST_HIT_POSE for vehicles/peds/objects (position +
    heading-only quaternion, cached per tick); materials 1 ground (friction 0.6), 2 vehicle paint (0.5). The probe
    ignores the copy too.
  - Drone model: procedural quad (body, battery, camera, arms, motors, blur discs > 1500 rpm, blades, broken props
    hidden) projected in render.cpp through the frame's camera, painter-sorted, hidden when a ray from the camera
    hits first; darker at night (GET_TIME_OF_DAY). No shadow.
  - No game UI in the drone's views: HIDE_HUD_AND_RADAR + HIDE_HELP_TEXT + CLEAR_PRINTS every tick (not just with
    the OSD switch). Kamikaze default ending length 2 s. Kamikaze removed from README (dev-gatekept per the doc).
  - Every native name in src/ was checked against the ScriptHook SDK's hash table (99/99).

- 2026-11-25 23:30-23:52: **First in-game run of the sync (user report: "a lot of buggy stuff" on F7, then a crash).**
  - **Frame layout bug (root cause of most of it):** CMake defined LIBFPV_KAMIKAZE for the mod while the shipped
    libfpv.dll is plain. libfpv.h then makes FPV_CHANNEL_COUNT 12, not 10, so every fpv_frame field after
    `channels` (OSD grid, impacts, wind, video_feed...) was read 8 bytes off: frame.wind showed `x 0 nan`, impacts
    named wrong entities. Older builds had the same define (fewer fields after channels then). Now kamikaze is the
    CMake option GTAIV_FPV_KAMIKAZE (OFF); plain builds compile the kamikaze code out (stand-in types in
    kamikaze.h), and fpvapi refuses a libfpv.dll of the other kind with a log line. gtasa-fpv likely has the same
    bug (it also defines LIBFPV_KAMIKAZE unconditionally).
  - Stand-in thrash: the shared settings (%APPDATA%\libfpv) had standInDistance 5 / return 5 from another game:
    Niko and his copy swapped every few frames. The return distance is now capped at 0.6x the start distance.
  - world::raycast drops hits with a non-finite distance (logged).
  - Loader: vectored exception handler logs fault address, module+offset and module return addresses on the stack
    ("crash?"). At startup it logs two harmless first-chance AVs from ScriptHookDotNet's CLR.
  - **Verified in game (self-tested, autotest + WinDrive keys):** video feed with 4:3 bars, analog look and aligned
    OSD; wind sane (~2.5-3 m/s NE); stand-in starts once at 5 m and holds through a 45 s flight 120 m out (city
    streams), Niko back home at the end; copy + pose logged; C -> chase view with the drone model drawn at the
    right place (small at 2 m; prop discs made brighter); F -> game camera on Niko with HUD/radar, he walks
    (WinDrive needs `scanmode on`: the game reads DirectInput); F7 -> back to the goggles, stand-in resumes; car and
    wall impacts during the stand-in; a hot reload mid-session. No crash in ~6 flights after the layout fix (the
    one crash seen came after the layout-bug session; not reproduced since).
  - The F8 overlay showed up open during one flight without a logged F8; not reproduced (maybe the user's key).

- 2026-11-25 23:55: **Kamikaze dev machine.** This PC has LIBFPV_KAMIKAZE_DEV=1 (~/.zshrc; libfpv CLAUDE.md "Kamikaze dev
  machines"): every local build and deploy is kamikaze. GTAIV_FPV_KAMIKAZE now defaults to ON when the variable is
  1/true (rdr2-fpv's pattern); CI doesn't set it, so release builds stay plain. deploy.sh picks
  libfpv/build/win32-kamikaze on such a machine and refuses a mod/libfpv kamikaze mismatch. The existing build/ was
  reconfigured with -DGTAIV_FPV_KAMIKAZE=ON (a cached OFF isn't changed by the new default). Rebuilt libfpv's
  win32-kamikaze at HEAD and deployed it. The in-game runs above were with the plain build.
  - User report from their first (layout-bug) session: picture all purple and frozen for seconds until an impact
    (the misread video_feed), F8 overlay opening by itself (seen once by me too). Every forwarded F8 press is now
    logged with GetAsyncKeyState, to tell a real F8 (another app, a radio that is also a keyboard?) from a bug.

- 2026-11-26 00:00: **Crash at the stand-in's start found and fixed.** User crash (kamikaze build, real radio) 8 ms after
  "stand-in: Niko goes to the drone": null read in GTAIV.exe+0x9f969a = memcpy (rep movsb) of 0x370 bytes from a null
  source, called by the CDrawPedDC constructor (RTTI `.?AVCDrawPedDC@@`, vtable RVA 0xa818b0; ctor 0x4dafc0, built
  at 0x5e30xx from ped+0x254 only when the object at ped+0x238 checks out, else null): a ped drawn without its data.
  The start changed two peds in one frame (CLEAR_CHAR_TASKS_IMMEDIATELY on the visible Niko, a new ped on his spot,
  an instant-blend anim on it). Now: tick N Niko hidden/frozen/no collision and moved (no immediate task clear),
  N+1 the copy created hidden, N+2 shown and posed (blend 8, phase set once it plays). 4 autotest runs in a row
  (start + end each), no crash. The user's first-session crash came at the same moment.
  - Shared settings: standInReturnDistance 5 -> 1 m at the user's request (backup settings.json.bak-before-standin).
    The game's defaults (fpv_pilot_set_support) are 20 m / 10 m.

- 2026-11-26 00:20: **No copy of Niko; the stand-in is the whole flight.** The crash at the stand-in's start was the
  copy (its CPed address was on the crashing stack). CPed+0x219 comes from the ped's creation info (CPed ctor
  0x5e26a0 copies it): 1 for the player's ped, 0 for CREATE_CHAR's. The ped draw code (0x5e2e60..) sends a
  player-model ped with +0x219 == 0 down the streamed-variation path (CDrawPedDC from the +0x238 block, which the
  player model never fills) -> null memcpy at 0x9f969a. Writing 1 into the copy's +0x219 sent it down the player
  path, which crashed at GTAIV.exe+0x60b9a2 reading null+0x384 (player-only data). So a script-made Niko can't be
  drawn. User's choice: stand-in on = Niko hidden at the drone from launch to the end of the flight (distances
  ignored, no swaps, no copy, no pose); off = Niko home in the pose. Respawn keeps him at the drone (LOAD_SCENE at
  the spawn) and spawns in front of his home spot. Also: hit cars/peds vanished (impacts on 0x2.../0x1... with no
  effect logged): probably the game removing what the hidden player was inside; Niko is now lifted 2.5 m while a
  drone probe sees a car/ped within 3 m (world::moving_thing_near), and effects logs "no longer exists". Pose:
  pilot::hold adopts a pose already playing instead of restarting it.

- 2026-11-26 00:40: **Cars handled like gtasa-fpv (libfpv INTEGRATION.md "Flying from a car").** With the stand-in on
  and Niko at the wheel: the car radio is kept on (its station read with GET_PLAYER_RADIO_STATION_INDEX, 255 = off, then
  SET_MOBILE_RADIO_ENABLED_DURING_GAMEPLAY + SET_MOBILE_PHONE_RADIO_STATE before WARP_CHAR_FROM_CAR_TO_COORD; held
  and retuned (max 3x) every tick; turned off again after he is back in the car unless the mobile radio was on before).
  The car is SET_CAR_AS_MISSION_CAR for the stand-in (MARK_CAR_AS_NO_LONGER_NEEDED after, unless a script already
  owned it), SET_CAR_ON_GROUND_PROPERLY when the 100 m hold ends. The stand-in's end always warps him back into the
  car (LOAD_SCENE at the car first when going home), so Y moves the car with him: SET_CAR_COORDINATES/HEADING/ON_GROUND
  and speed 0 at the drone; also with the stand-in off (it used to warp him out). Deployed hot (game running,
  paused at the time), NOT tested in game per the user.

- 2026-11-26 00:57: **Respawns fell through the map.** User's log: every respawn while standing in far from the spawn
  (kamikaze "then respawn", R) put the drone on the road at z 1.7 and it sank to -150: the spot's collision had
  streamed out with Niko at the old drone. Now respawn requests collision + LOAD_SCENE at the spot (his home spot when
  the spawn is re-measured after the pilot view), puts Niko there at once, and the drone is held on the spawn
  (fpv_drone_reset every tick, before fpv_drone_tick) until world::raycast finds the ground within 1 m above / 3 m
  below it, then starts on that ground; after 3 s it is let go with a log line ("spawn: ..."). Stand-in placement uses
  the spawn, not the stale frames, while it waits. Launches go through the same check. Deployed hot, not tested in
  game.

## Next session — start here
0. Ask the user how the fixed build flies with the real radio. Still not checked in game:
   - cars: the radio playing on while standing in (and no retune static), Y taking the car along (stand-in on and
     off), the car still there after a long flight, set down cleanly after the 100 m hold;
   - the stand-in: does SET_CHAR_COORDINATES_NO_OFFSET move a frozen Niko, does the copy appear (CREATE_CHAR with
     the player model) in his clothes, does the AI shoot at the drone (log "damage: Niko standing in lost"), does
     Niko land back on his feet / in his car;
   - the pose (cellphone_text) on Niko and the copy, and its seamless hand-over;
   - the pilot view: game camera back, F not entering a car, F7 back to the drone, listener panning;
   - the drone model's projection (SET_CAM_FOV assumed vertical) and the pilot view's GET_CAM_ROT order;
   - the video feed (F8 > Camera > Video feed) and the OSD in the signal;
   - the controls in F8 > Controls / radio setup; the overlay's text fields;
   - water damage + drag, wind strength, explosion blasts, heli downwash;
   - the old-install clean-up (libfpv/old) on the user's machine.
1. Check the overlay with real mouse use (GTA may clip/hide the cursor; GetCursorPos may stick) - needs the user.
2. Polish: frame.altitude -1 when > a few m up (libfpv probe range?); tune effect strengths with the user.
3. Not done (no way found yet): the game's own sounds heard from the pilot (FPV_DRONE_HEARD_FROM_PILOT; IV has no
   listener native), game sounds for reactions (frontend sound names unknown besides GENERAL_GPS_BONG), Esc that
   closes the overlay also reaching the pause menu, a game-rendered drone model with a shadow, hurting peds through
   a damage event naming the player (IV's DAMAGE_CHAR has no attacker).
4. Delete GTAIV/libfpv/autotest.txt before handing the game back to the user.
