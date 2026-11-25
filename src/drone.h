// The FPV drone session: libfpv drone + scripted game camera.
#pragma once

#include <libfpv.h>

namespace drone {

// F7: launches the drone from Niko, or ends the flight back at Niko; from the
// pilot view (F), back to the drone left in the world. Script thread only.
void toggle();

// Once per script tick. Script thread only.
void tick();

bool active();

// A drone is up and the view is back on Niko (F): the game's camera, HUD and
// controls, the drone left in the world.
bool pilot_view();

// Ends the flight with Niko standing under the drone (Y, F8 > Fly button). Script thread only.
void move_niko_here();

// The frame from the latest tick (struct_size 0 before the first one).
// Script thread only.
const fpv_frame &last_frame();

// Copies the latest frame for the lens pass and the OSD; false when no drone
// is flying or a third-person camera is on. no_signal: the warhead went off
// and the goggles show static. Any thread.
bool osd_frame(fpv_frame *out, bool *no_signal);

// What the drone model pass needs: the drone's pose and props, and the camera
// this frame renders from. False in the FPV view (the lens is inside the
// frame), when the drone is hidden behind something, and when no drone is up.
// Any thread.
struct ModelPose {
    float pos[3], right[3], up[3], forward[3];  // body axes, world
    float motor[4][3];                           // hubs, body-local (right, forward, up), metres
    float rpm[4], spin[4];                       // spin: the blades' angle, radians
    bool prop_gone[4];
    float prop_radius;
    float eye[3], eye_right[3], eye_up[3], eye_forward[3];
    float fov_y;  // degrees
    float light;  // 0..1: night is darker
};
bool model_pose(ModelPose *out);

// Host-injected radio channels (AETR, AUX1..8, -1..1), overriding devices for 0.5 s.
void inject_channels(const float channels[FPV_CHANNEL_COUNT]);

// Test knobs for the autotest (0 / negative = normal behaviour).
struct Tuning {
    float fov = 0;        // vertical degrees, overrides libfpv's FPV FOV
    float near_clip = 0;  // overrides kNearClip
    bool osd = true;
    bool armed_flight = true;  // false: the autotest leaves the drone on the ground
    bool hover = false;        // autotest hovers 2.5 m up without moving
    bool move_niko = false;    // autotest ends with "Move Niko here" instead of F7
    float seconds = 20;        // autotest flight length
    bool long_range = false;   // autotest climbs 35 m before cruising, for long-range streaming tests
    bool in_car = false;       // autotest puts Niko in the target car before the launch
    bool osd_off = false;      // autotest flies with the OSD disable switch (AUX6) on
    bool bomb = false;          // autotest turns the bomb arm switch (AUX7) on in flight
    bool explode_switch = false;  // autotest fires the explode switch (AUX8) instead of hitting the target
    int after_blast = -1;       // ending view: 0 signal lost, 1 cutscene; -1 = the player's (F8 > Kamikaze)
    int ending_then = -1;       // fpv_kamikaze_ending_then; -1 = the player's
    float ending_seconds = -1;  // ending view length; -1 = the player's
};
Tuning &tuning();

// The game's camera Euler angles (degrees) for a view basis in game coords.
void basis_to_euler(const float forward[3], const float up[3], const float right[3], float *pitch, float *roll,
                    float *heading);

}  // namespace drone
