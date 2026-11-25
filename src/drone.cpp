#include "drone.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include "audio.h"
#include "damage.h"
#include "effects.h"
#include "fpvapi.h"
#include "kamikaze.h"
#include "log.h"
#include "pilot.h"
#include "render.h"
#include "scripthook.h"
#include "video.h"
#include "wind.h"
#include "world.h"

namespace drone {
namespace {

constexpr float kRadToDeg = 57.2957795f;
constexpr float kNearClip = 0.2f;      // with clear space in front of the lens
constexpr float kMinNearClip = 0.02f;  // when the lens almost touches something
constexpr float kWidestAspect = 2.4f;  // frustum probes cover up to 21:9
constexpr float kPedFeet = 1.0f;       // GET_CHAR_COORDINATES is the pelvis, about this far above the feet
constexpr float kCutsceneDistance = 14.0f;  // camera back from the blast, horizontally
constexpr float kCutsceneHeight = 5.0f;     // and up
constexpr float kWarheadBlastRadius = 3.0f; // the warhead is grenade-sized (kamikaze.cpp)

// Standing in (libfpv INTEGRATION.md "Standing in for the drone"): Niko at
// the drone, hidden, for the whole flight (no copy at home: see standin_update).
constexpr float kStandInHealth = 1000.0f;  // Niko's health while he stands in, topped up every tick
constexpr float kAimFallback = 0.35f;      // spine above the root, m, until the bones are known
constexpr float kPlaceStep = 0.25f;        // re-place Niko once the drone moved this far from him
constexpr float kCarHoldDistance = 100.0f;
constexpr float kNearThingLift = 2.5f;     // m, Niko's spine above the drone while a car or ped is near it       // the return distance is at most this share of the start distance // past this the ground under his car may stream out: hold it

using fpvapi::fpv;

enum class View { Fpv, Chase, Los, Count };
constexpr uint32_t kMoveNikoAction = 1;  // F8 > Fly button id

fpv_drone *g_drone = nullptr;
uint32_t g_cam = 0;
uint32_t g_player = 0;
View g_view = View::Fpv;
bool g_pilot_view = false;   // F: the view is back on Niko, the drone left in the world
bool g_pilot_moved = false;  // the pilot view was used: Niko may have walked off from the launch spot
int g_pilot_key_hold = 0;    // ticks to keep Niko's controls off after F (until the key is up)
float g_spawn_pos[3];
float g_spawn_rot[4];
LARGE_INTEGER g_last_tick;
double g_first_sim_time = -1;  // sim clock at the first frame of this flight
fpv_frame g_frame{};
bool g_have_frame = false;
float g_near_clip = 0, g_fov = 0;  // last values sent to the game, for the trace
float g_spin[4] = {};              // the props' angle, for the model

// The camera this frame renders from (the scripted camera in the drone's views).
struct Shot {
    bool on = false;
    float pos[3], forward[3], up[3], right[3], fov;
};
Shot g_shot;

std::mutex g_osd_mutex;  // guards what the render thread reads below
fpv_frame g_osd_frame{};
bool g_osd_visible = false;
bool g_osd_no_signal = false;
ModelPose g_model{};
bool g_model_on = false;

// Kamikaze builds: after the warhead went off, static in the goggles or an
// outside shot of the blast for the player's duration, then a respawn, the
// end of the flight or nothing (the view stays until the player acts). The
// player's own R / F7 / Y work at once and replace what comes next.
struct Ending {
    bool on = false;
    kamikaze::Ending choice;
    float elapsed = 0;  // game time: stops while the game is paused
};
Ending g_ending;

// The game streams map detail, LODs, collision, traffic and its AI around
// Niko. Past the stand-in distance, Niko goes to the drone (invisible,
// frozen, without collision) and the world treats the drone as him: the city
// streams around it, the police and gangs see, chase and shoot it, and what
// hurts him breaks the drone's props. A copy of him stays at the piloting
// spot, in the pose (or at the wheel of his car).
struct StandIn {
    bool on = false;
    uint32_t niko = 0;
    uint32_t car = 0;    // the car he drove, 0 on foot
    bool car_held = false;
    float home[3];       // his root (GET_CHAR_COORDINATES) at the spot
    float heading;
    float at[3];         // the drone position he was last placed for
    float health_before;
    float expected_health;
    float aim = kAimFallback;  // his spine above his root
    float correction = 0;      // what SET_CHAR_COORDINATES_NO_OFFSET is off by, measured
    float placed_root = 0;     // the root height asked for last
    bool placed = false;
    bool lifted = false;       // Niko above the drone, clear of a car or ped next to it
};
StandIn g_standin;
bool g_standin_refused_logged = false;

uint32_t player_index() {
    return sh::call<uint32_t>(N("CONVERT_INT_TO_PLAYERINDEX"), sh::call<uint32_t>(N("GET_PLAYER_ID")));
}

uint32_t niko() {
    uint32_t ped = 0;
    sh::call(N("GET_PLAYER_CHAR"), g_player ? g_player : player_index(), &ped);
    return ped;
}

void print(const char *text, uint32_t ms = 4000) {
    sh::call(N("PRINT_STRING_WITH_LITERAL_STRING_NOW"), "STRING", text, ms, 1u);
}

bool game_has_focus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

float dist(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// v rotated by quaternion q {x, y, z, w}.
void rotate(const float q[4], const float v[3], float out[3]) {
    float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
          tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// Keys for libfpv: the keyboard fallback (W/S throttle, A/D yaw, arrows
// pitch/roll, Enter arms, T turtle, B beeper; kamikaze builds K bomb arm, X
// explode), the overlay's keys, and the game controls' keys (C, R, F, Y by
// default, any letter or digit once the player rebinds them in F8 > Controls).
// Letters and digits by scan code: libfpv wants the physical key, so on
// AZERTY the throttle keys stay under the left hand. While the overlay takes
// input, the same presses also go in as typed text, in the player's layout.
void forward_keys() {
    struct Key {
        int vk;   // fixed virtual key, or 0 for a scan code
        int fpv;
        UINT sc;
    };
    static Key keys[64];
    static int count = 0;
    if (!count) {
        static const char kLetters[] = "QWERTYUIOPASDFGHJKLZXCVBNM";
        static const UINT kLetterSc[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1E, 0x1F, 0x20,
                                         0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32};
        for (int i = 0; i < 26; ++i) keys[count++] = {0, kLetters[i], kLetterSc[i]};
        for (int i = 0; i < 10; ++i) keys[count++] = {0, '0' + (i + 1) % 10, UINT(0x02 + i)};  // 1..9, 0
        const Key specials[] = {
            {VK_RETURN, FPV_KEY_ENTER, 0}, {VK_LEFT, FPV_KEY_LEFT, 0},   {VK_RIGHT, FPV_KEY_RIGHT, 0},
            {VK_UP, FPV_KEY_UP, 0},        {VK_DOWN, FPV_KEY_DOWN, 0},   {VK_F8, FPV_KEY_F8, 0},
            {VK_ESCAPE, FPV_KEY_ESCAPE, 0}, {VK_TAB, FPV_KEY_TAB, 0},    {VK_BACK, FPV_KEY_BACKSPACE, 0},
            {VK_SPACE, FPV_KEY_SPACE, 0},  {VK_DELETE, FPV_KEY_DELETE, 0}, {VK_HOME, FPV_KEY_HOME, 0},
            {VK_END, FPV_KEY_END, 0},      {VK_LSHIFT, FPV_KEY_LSHIFT, 0}, {VK_RSHIFT, FPV_KEY_RSHIFT, 0},
            {VK_LCONTROL, FPV_KEY_LCTRL, 0}, {VK_RCONTROL, FPV_KEY_RCTRL, 0},
        };
        for (const Key &k : specials) keys[count++] = k;
    }
    static bool was_down[64];
    static bool primed = false;  // a key already held when we start is not a press
    bool focus = game_has_focus();
    // the layout is per thread: take the game window's, not this script thread's
    HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    bool typing = fpv.fpv_overlay_wants_input() != 0;
    // In the pilot view the keys walk and drive Niko: they reach libfpv only
    // while the overlay is open (and F8 to open it), so the keyboard fallback
    // doesn't fly the drone off while he walks. A radio still flies it.
    bool walking = pilot_view() && !typing;
    for (int i = 0; i < count; ++i) {
        int vk = keys[i].sc ? (int)MapVirtualKeyExW(keys[i].sc, MAPVK_VSC_TO_VK, layout) : keys[i].vk;
        bool down = focus && vk && (GetAsyncKeyState(vk) & 0x8000) != 0 && !(walking && keys[i].fpv != FPV_KEY_F8);
        if (primed && down != was_down[i]) {
            fpv.fpv_input_key(keys[i].fpv, down);
            if (keys[i].fpv == FPV_KEY_F8 && down) {  // the overlay toggles: say so, it has opened "by itself"
                static int reported = 0;
                if (reported++ < 50)
                    fpvlog::write("F8 down (GetAsyncKeyState 0x%04x): overlay %s", unsigned(GetAsyncKeyState(VK_F8)) & 0xffffu,
                                  fpv.fpv_overlay_is_open() ? "opened" : "closed");
            }
            // typed text for the overlay's text fields (letters, digits, space)
            if (down && typing && (keys[i].sc || vk == VK_SPACE)) {
                BYTE state[256] = {};
                for (int k : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_MENU})
                    if (GetAsyncKeyState(k) & 0x8000) state[k] = 0x80;
                if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 1;
                WCHAR wide[4];
                int n = ToUnicodeEx(UINT(vk), keys[i].sc, state, wide, 4, 0, layout);
                if (n > 0) {
                    char utf8[16];
                    int m = WideCharToMultiByte(CP_UTF8, 0, wide, n, utf8, sizeof utf8 - 1, nullptr, nullptr);
                    if (m > 0) {
                        utf8[m] = 0;
                        fpv.fpv_overlay_text(utf8);
                    }
                }
            }
        }
        was_down[i] = down;
    }
    primed = true;
}

bool key_down_scan(UINT sc) {
    HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    int vk = int(MapVirtualKeyExW(sc, MAPVK_VSC_TO_VK, layout));
    return vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// The FPV camera rides a few cm above the road: a fixed near plane cuts the
// ground away under the lens and the game shows whatever was left in that
// part of the frame. Probe the view frustum (centre, edge midpoints and
// corners) and pull the near plane in front of the closest geometry.
float near_clip_for(const fpv_camera &c, float fov_y) {
    const float ty = std::tan(fov_y * 0.5f / kRadToDeg), tx = ty * kWidestAspect;
    float nearest = kNearClip / 0.8f;  // depth along the view axis
    for (int sy = -1; sy <= 1; ++sy)
        for (int sx = -1; sx <= 1; ++sx) {
            float d[3], len = 0;
            for (int i = 0; i < 3; ++i) {
                d[i] = c.forward[i] + c.right[i] * tx * float(sx) + c.up[i] * ty * float(sy);
                len += d[i] * d[i];
            }
            len = std::sqrt(len);
            for (float &v : d) v /= len;
            fpv_raycast_hit hit{};
            // depth = distance * cos(angle to the axis) = distance / len
            if (world::raycast(nullptr, c.position, d, nearest * len, &hit) && hit.distance / len < nearest)
                nearest = hit.distance / len;
        }
    float n = nearest * 0.8f;
    return n < kMinNearClip ? kMinNearClip : n > kNearClip ? kNearClip : n;
}

// libfpv widens the FOV for its fisheye so the top and bottom edges of the
// distorted image show the FOV the player chose, but for the camera's
// aspect_hint (4:3 analog). Redo it for the aspect the picture really has.
float lens_edge(const fpv_camera &c, float aspect) {
    float r2 = 1 / (1 + aspect * aspect);  // squared radius of the top edge centre
    float zoom = c.distortion_zoom > 0 ? c.distortion_zoom : 1;
    return (1 + c.distortion_k1 * r2 + c.distortion_k2 * r2 * r2) / zoom;
}
// With the video feed's resolution effect, the goggles show the camera's own
// picture (4:3 or 16:9, black bars) cut from the middle of the game's frame:
// the lens works on that aspect, and a picture wider than the screen needs a
// taller frame.
float lens_fov(const fpv_camera &c) {
    float out = render::aspect(), taller = 1;
    const fpv_video_feed &vf = g_frame.video_feed;
    if (g_have_frame && (video::flags(g_frame) & FPV_VIDEO_FEED_RESOLUTION) && vf.picture_aspect > 0.1f &&
        video::usable()) {
        out = vf.picture_aspect;
        if (out > render::aspect()) taller = out / render::aspect();
    }
    float t = std::tan(c.fov_y_degrees * 0.5f / kRadToDeg);
    if ((c.distortion_k1 != 0 || c.distortion_k2 != 0) && c.aspect_hint > 0)
        t *= lens_edge(c, c.aspect_hint) / lens_edge(c, out);
    float fov = 2 * std::atan(t * taller) * kRadToDeg;
    return fov > 170 ? 170 : fov;
}

void set_shot(const float pos[3], const float f[3], const float u[3], const float r[3], float fov) {
    for (int i = 0; i < 3; ++i) {
        g_shot.pos[i] = pos[i];
        g_shot.forward[i] = f[i];
        g_shot.up[i] = u[i];
        g_shot.right[i] = r[i];
    }
    g_shot.fov = fov;
    g_shot.on = true;
}

void apply_camera(const fpv_camera &c, bool fpv_view) {
    float pitch, roll, heading;
    basis_to_euler(c.forward, c.up, c.right, &pitch, &roll, &heading);
    sh::call(N("SET_CAM_POS"), g_cam, c.position[0], c.position[1], c.position[2]);
    sh::call(N("SET_CAM_ROT"), g_cam, pitch, roll, heading);
    float fov = fpv_view && tuning().fov > 0 ? tuning().fov : fpv_view ? lens_fov(c) : c.fov_y_degrees;
    sh::call(N("SET_CAM_FOV"), g_cam, fov);
    float near_clip = tuning().near_clip > 0 ? tuning().near_clip : fpv_view ? near_clip_for(c, fov) : kNearClip;
    sh::call(N("SET_CAM_NEAR_CLIP"), g_cam, near_clip);
    g_near_clip = near_clip;
    g_fov = fov;
    set_shot(c.position, c.forward, c.up, c.right, fov);
}

// The scripted camera on (the drone's views) or off (the pilot view: the game's own).
void script_camera(bool on) {
    sh::call(N("SET_CAM_ACTIVE"), g_cam, on);
    sh::call(N("ACTIVATE_SCRIPTED_CAMS"), on, on);
}

// The game camera's position and basis: R = Rz(heading) Ry(roll) Rx(pitch),
// local x right, y forward, z up (basis_to_euler's inverse).
bool game_camera(float pos[3], float right[3], float forward[3], float up[3], float *fov) {
    uint32_t cam = 0;
    sh::call(N("GET_GAME_CAM"), &cam);
    if (!cam) return false;
    float rot[3] = {};
    sh::call(N("GET_CAM_POS"), cam, &pos[0], &pos[1], &pos[2]);
    sh::call(N("GET_CAM_ROT"), cam, &rot[0], &rot[1], &rot[2]);
    sh::call(N("GET_CAM_FOV"), cam, fov);
    float p = rot[0] / kRadToDeg, r = rot[1] / kRadToDeg, h = rot[2] / kRadToDeg;
    float cp = std::cos(p), sp = std::sin(p), cr = std::cos(r), sr = std::sin(r), ch = std::cos(h), shh = std::sin(h);
    const float Rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    const float Ry[3][3] = {{cr, 0, sr}, {0, 1, 0}, {-sr, 0, cr}};
    const float Rz[3][3] = {{ch, -shh, 0}, {shh, ch, 0}, {0, 0, 1}};
    float A[3][3] = {}, M[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) A[i][j] += Ry[i][k] * Rx[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) M[i][j] += Rz[i][k] * A[k][j];
    for (int i = 0; i < 3; ++i) right[i] = M[i][0], forward[i] = M[i][1], up[i] = M[i][2];
    if (!(*fov > 1 && *fov < 170)) *fov = 50;
    return true;
}

// A quaternion turning local x/y/z onto the columns right/forward/up.
void basis_to_quat(const float r[3], const float f[3], const float u[3], float q[4]) {
    float m00 = r[0], m01 = f[0], m02 = u[0], m10 = r[1], m11 = f[1], m12 = u[1], m20 = r[2], m21 = f[2], m22 = u[2];
    float tr = m00 + m11 + m22;
    if (tr > 0) {
        float k = std::sqrt(tr + 1.0f) * 2;
        q[3] = 0.25f * k, q[0] = (m21 - m12) / k, q[1] = (m02 - m20) / k, q[2] = (m10 - m01) / k;
    } else if (m00 > m11 && m00 > m22) {
        float k = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q[3] = (m21 - m12) / k, q[0] = 0.25f * k, q[1] = (m01 + m10) / k, q[2] = (m02 + m20) / k;
    } else if (m11 > m22) {
        float k = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q[3] = (m02 - m20) / k, q[0] = (m01 + m10) / k, q[1] = 0.25f * k, q[2] = (m12 + m21) / k;
    } else {
        float k = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q[3] = (m10 - m01) / k, q[0] = (m02 + m20) / k, q[1] = (m12 + m21) / k, q[2] = 0.25f * k;
    }
}

// ---------------------------------------------------------------------------
// Standing in

// Niko at the drone: hidden, frozen, without collision, but seen by everyone
// (no "ignore the player": the police and gangs go after the drone).
void set_stand_in_state(uint32_t ped, bool on) {
    sh::call(N("FREEZE_CHAR_POSITION"), ped, on);
    sh::call(N("SET_CHAR_VISIBLE"), ped, !on);
    sh::call(N("SET_CHAR_COLLISION"), ped, !on);
}

// The spine (what the AI aims at) on the drone, not his root: measured from
// his bones when they are in place.
void place_niko(const float drone_pos[3]) {
    StandIn &s = g_standin;
    float root[3];
    sh::call(N("GET_CHAR_COORDINATES"), s.niko, &root[0], &root[1], &root[2]);
    if (s.placed) {  // how far the last placement landed from what was asked
        float err = s.placed_root - root[2];
        if (std::fabs(err) < 2.0f) s.correction += 0.5f * err;
    }
    float bone[4] = {};
    sh::call(N("GET_PED_BONE_POSITION"), s.niko, 0x4B3u /* BONE_SPINE1 */, 0.0f, 0.0f, 0.0f, bone);
    float aim = bone[2] - root[2];
    if (std::fabs(bone[0] - root[0]) < 1.5f && std::fabs(bone[1] - root[1]) < 1.5f && aim > -0.5f && aim < 1.5f)
        s.aim = aim;
    // Near a car or a ped Niko's body would be inside it (no collision, on
    // the drone) and the game removed them: lift him clear of them then.
    float lift = world::moving_thing_near() ? kNearThingLift : 0.0f;
    if ((lift > 0) != s.lifted) {
        s.lifted = lift > 0;
        fpvlog::write("stand-in: Niko %s", s.lifted ? "lifted above the drone (a car or ped is near it)" : "back on the drone");
    }
    s.placed_root = drone_pos[2] - s.aim + lift;
    sh::call(N("SET_CHAR_COORDINATES_NO_OFFSET"), s.niko, drone_pos[0], drone_pos[1], s.placed_root + s.correction);
    s.placed = true;
    for (int i = 0; i < 3; ++i) s.at[i] = drone_pos[i];
}

// Why Niko can't stand in now (null when he can).
const char *stand_in_refusal(uint32_t ped) {
    if (!(fpvapi::pilot_config().features & FPV_PILOT_STAND_IN)) return "off in F8 > Fly > Pilot";
    if (sh::call<bool>(N("IS_CHAR_IN_ANY_CAR"), ped)) {
        uint32_t car = 0, driver = 0;
        sh::call(N("GET_CAR_CHAR_IS_USING"), ped, &car);
        if (car) sh::call(N("GET_DRIVER_OF_CAR"), car, &driver);
        if (driver != ped) return "a passenger";
        if (sh::call<bool>(N("GET_MISSION_FLAG"))) return "on a mission in a car";
    }
    return nullptr;
}

void standin_begin(const float drone_pos[3]) {
    uint32_t ped = niko();
    if (const char *why = stand_in_refusal(ped)) {
        if (!g_standin_refused_logged) fpvlog::write("stand-in: Niko stays home (%s)", why);
        g_standin_refused_logged = true;
        return;
    }
    StandIn s{};
    s.niko = ped;
    sh::call(N("GET_CHAR_COORDINATES"), ped, &s.home[0], &s.home[1], &s.home[2]);
    sh::call(N("GET_CHAR_HEADING"), ped, &s.heading);
    if (sh::call<bool>(N("IS_CHAR_IN_ANY_CAR"), ped)) sh::call(N("GET_CAR_CHAR_IS_USING"), ped, &s.car);
    // the pose goes over to the copy where it is, so the hand-over doesn't show
    if (s.car) sh::call(N("WARP_CHAR_FROM_CAR_TO_COORD"), ped, drone_pos[0], drone_pos[1], drone_pos[2]);
    pilot::forget(ped);  // he keeps whatever he plays, hidden at the drone
    uint32_t health = 0;
    sh::call(N("GET_CHAR_HEALTH"), ped, &health);
    s.health_before = float(health);
    set_stand_in_state(ped, true);
    sh::call(N("SET_CHAR_HEALTH"), ped, uint32_t(kStandInHealth));
    sh::call(N("GET_CHAR_HEALTH"), ped, &health);  // what the game kept of it
    s.expected_health = float(health);
    s.on = true;
    g_standin = s;
    place_niko(drone_pos);
    fpvlog::write("stand-in: Niko goes to the drone from %.2f %.2f %.2f (car %u, health %.0f, held at %.0f)",
                  s.home[0], s.home[1], s.home[2], s.car, s.health_before, s.expected_health);
}

// Niko comes back from the drone: to his spot (and his car), or (go_home
// false, Move Niko here) he stays where he is.
void standin_end(bool go_home) {
    StandIn &s = g_standin;
    if (!s.on) return;
    s.on = false;
    uint32_t ped = s.niko;
    set_stand_in_state(ped, false);
    sh::call(N("SET_CHAR_HEALTH"), ped, uint32_t(s.health_before > 100 ? s.health_before : 200));
    bool car = s.car && sh::call<bool>(N("DOES_VEHICLE_EXIST"), s.car);
    if (car && s.car_held) sh::call(N("FREEZE_CAR_POSITION"), s.car, false);
    if (!go_home) {
        fpvlog::write("stand-in: over, Niko stays at the drone");
        return;
    }
    const float *h = s.home;
    sh::call(N("LOAD_SCENE"), h[0], h[1], h[2]);
    if (car) {
        sh::call(N("WARP_CHAR_INTO_CAR"), ped, s.car);
        fpvlog::write("stand-in: over, Niko back in car %u", s.car);
    } else {
        sh::call(N("SET_CHAR_COORDINATES"), ped, h[0], h[1], h[2]);
        sh::call(N("SET_CHAR_HEADING"), ped, s.heading);
        fpvlog::write("stand-in: over, Niko back at %.2f %.2f %.2f%s", h[0], h[1], h[2],
                      s.car ? " (his car is gone)" : "");
    }
}

void standin_update(const fpv_frame &f) {
    if (g_pilot_view) return;
    // On, Niko rides with the drone for the whole flight, hidden: GTA IV
    // can't draw a copy of the player's ped to stand at the spot (a script's
    // ped with his model crashes the renderer, see MODLOG), so nobody stays
    // home, and the distances aren't used. Off, he stays home as himself.
    bool want = (fpvapi::pilot_config().features & FPV_PILOT_STAND_IN) != 0;
    StandIn &s = g_standin;
    if (s.on && !want) {  // the player turned it off mid-flight
        standin_end(true);
        return;
    }
    if (!s.on) {
        if (want) standin_begin(f.position);
        return;
    }
    if (dist(f.position, s.at) > kPlaceStep || world::moving_thing_near() != s.lifted) place_niko(f.position);
    if (s.car && sh::call<bool>(N("DOES_VEHICLE_EXIST"), s.car)) {
        bool hold = dist(f.position, s.home) > kCarHoldDistance;
        if (hold != s.car_held) {
            sh::call(N("FREEZE_CAR_POSITION"), s.car, hold);
            s.car_held = hold;
            fpvlog::write("stand-in: car %u %s", s.car, hold ? "held (the drone is far)" : "free");
        }
    }
    // What hurts Niko at the drone hurts the drone; he himself doesn't die of it.
    uint32_t health = 0;
    sh::call(N("GET_CHAR_HEALTH"), s.niko, &health);
    if (float(health) < s.expected_health) {
        damage::stand_in_hit(g_drone, f, s.expected_health - float(health));
        sh::call(N("SET_CHAR_HEALTH"), s.niko, uint32_t(kStandInHealth));
        sh::call(N("GET_CHAR_HEALTH"), s.niko, &health);
        s.expected_health = float(health);
    }
}

// ---------------------------------------------------------------------------

// The pilot's feet: his spot while he stands in, else where he is.
void pilot_feet(float feet[3]) {
    if (g_standin.on) {
        for (int i = 0; i < 3; ++i) feet[i] = g_standin.home[i];
    } else {
        sh::call(N("GET_CHAR_COORDINATES"), niko(), &feet[0], &feet[1], &feet[2]);
    }
    feet[2] -= kPedFeet;
}

// Betaflight's home arrow, home distance and GPS Rescue lead back to Niko,
// even if he walked off (the pilot view) or his car rolled away; the LOS
// camera, the video link and the ear follow him too.
void update_pilot() {
    float feet[3];
    pilot_feet(feet);
    fpv.fpv_drone_set_pilot_position(g_drone, feet);
    float home[3] = {feet[0], feet[1], feet[2] + kPedFeet};
    fpv.fpv_gps_set_home(home);
}

// The piloting pose while the drone is flown from its views and the radio is
// in use (the player can turn it off in F8 > Fly > Pilot), on Niko or his
// copy. Never in the pilot view, and not in a car.
void update_pose(const fpv_frame &f) {
    if (g_standin.on) return;  // he is hidden at the drone
    uint32_t target = niko();
    bool on_foot = !sh::call<bool>(N("IS_CHAR_IN_ANY_CAR"), target);
    bool want = !g_pilot_view && (f.flags & FPV_DRONE_RADIO_ACTIVE) &&
                (fpvapi::pilot_config().features & FPV_PILOT_POSE) && on_foot;
    pilot::hold(target, want);
}

// No game UI over the flight (libfpv INTEGRATION.md "No game UI in FPV"):
// the HUD, radar, help boxes and subtitles go in the drone's views. GTA IV
// has no per-frame hide for subtitles: the ones printed meanwhile are dropped.
void hide_game_ui() {
    sh::call(N("HIDE_HUD_AND_RADAR_THIS_FRAME"));
    sh::call(N("HIDE_HELP_TEXT_THIS_FRAME"));
    sh::call(N("CLEAR_PRINTS"));
}

// Where a drone starts (a launch, or a respawn after the pilot view): two
// metres in front of Niko (four in front of his car), resting on the ground,
// facing his way.
bool place_spawn(float pilot[3], float *heading_deg) {
    uint32_t ped = niko();
    uint32_t car = 0;
    if (sh::call<bool>(N("IS_CHAR_IN_ANY_CAR"), ped)) sh::call(N("GET_CAR_CHAR_IS_USING"), ped, &car);
    float heading = 0;
    if (g_standin.on) {  // Niko is at the drone: the spot (or the car) he left
        car = g_standin.car && sh::call<bool>(N("DOES_VEHICLE_EXIST"), g_standin.car) ? g_standin.car : 0;
        for (int i = 0; i < 3; ++i) pilot[i] = g_standin.home[i];
        heading = g_standin.heading;
        if (car) sh::call(N("GET_CAR_HEADING"), car, &heading);
    } else if (car) {
        sh::call(N("GET_CAR_COORDINATES"), car, &pilot[0], &pilot[1], &pilot[2]);
        sh::call(N("GET_CAR_HEADING"), car, &heading);
    } else {
        sh::call(N("GET_CHAR_COORDINATES"), ped, &pilot[0], &pilot[1], &pilot[2]);
        sh::call(N("GET_CHAR_HEADING"), ped, &heading);
    }
    float h = heading / kRadToDeg, ahead = car ? 4.0f : 2.0f;
    g_spawn_pos[0] = pilot[0] - std::sin(h) * ahead;
    g_spawn_pos[1] = pilot[1] + std::cos(h) * ahead;
    float origin[3] = {g_spawn_pos[0], g_spawn_pos[1], pilot[2] + 1.0f}, down[3] = {0, 0, -1};
    fpv_raycast_hit hit{};
    bool ground = world::raycast(nullptr, origin, down, 5.0f, &hit) != 0;
    if (ground) {
        g_spawn_pos[2] = origin[2] - hit.distance;
    } else {
        float z = 0;
        sh::call(N("GET_GROUND_Z_FOR_3D_COORD"), g_spawn_pos[0], g_spawn_pos[1], pilot[2] + 1.0f, &z);
        g_spawn_pos[2] = (z != 0 && z < pilot[2] + 1.0f) ? z : pilot[2] - kPedFeet;
    }
    g_spawn_rot[0] = 0;
    g_spawn_rot[1] = 0;
    g_spawn_rot[2] = std::sin(h / 2);
    g_spawn_rot[3] = std::cos(h / 2);
    *heading_deg = heading;
    return ground;
}

fpv_view libfpv_view() {
    if (g_pilot_view) return FPV_VIEW_PILOT;
    if (g_ending.on && g_ending.choice.view == FPV_KAMIKAZE_VIEW_CUTSCENE) return FPV_VIEW_LOS;  // an outside shot
    return g_view == View::Chase ? FPV_VIEW_CHASE : g_view == View::Los ? FPV_VIEW_LOS : FPV_VIEW_FPV;
}

void start() {
    if (!fpvapi::load()) {
        print("FPV: libfpv failed to load, see GTAIV\\libfpv\\libfpv_gtaiv.log");
        return;
    }
    g_player = player_index();
    uint32_t ped = niko();
    float pilot[3], heading = 0;
    bool ground = place_spawn(pilot, &heading);

    fpv_drone_desc desc;
    fpv.fpv_drone_desc_defaults(&desc);
    for (int i = 0; i < 3; ++i) desc.position[i] = g_spawn_pos[i];
    for (int i = 0; i < 4; ++i) desc.rotation[i] = g_spawn_rot[i];
    desc.pilot_position[0] = pilot[0];
    desc.pilot_position[1] = pilot[1];
    desc.pilot_position[2] = pilot[2] - kPedFeet;
    fpv_result r = fpv.fpv_drone_create(&desc, &g_drone);
    if (r != FPV_OK) {
        fpvlog::write("fpv_drone_create: %s (%s)", fpv.fpv_result_string(r), fpv.fpv_last_error());
        print("FPV: drone creation failed, see GTAIV\\libfpv\\libfpv_gtaiv.log");
        g_drone = nullptr;
        return;
    }
    world::ignore_ped(ped);
    fpv.fpv_world_set_raycast_callback_ex(&world::raycast_ex, nullptr);
#if defined(LIBFPV_KAMIKAZE)
    if (fpvapi::kamikaze()) {  // detonations of earlier drones are history
        fpv_detonation d;
        d.struct_size = sizeof d;
        while (fpv.fpv_kamikaze_poll_detonation(&d)) {}
    }
#endif
    g_ending = Ending{};
    g_standin = StandIn{};
    g_standin_refused_logged = false;
    float home[3] = {pilot[0], pilot[1], pilot[2]};
    fpv.fpv_gps_set_home(home);
    wind::reset();
    damage::reset();

    sh::call(N("SET_PLAYER_CONTROL"), g_player, false);
    sh::call(N("CREATE_CAM"), 14u, &g_cam);
    sh::call(N("SET_CAM_PROPAGATE"), g_cam, true);
    script_camera(true);
    QueryPerformanceCounter(&g_last_tick);
    g_first_sim_time = -1;
    g_have_frame = false;
    g_shot.on = false;
    g_view = View::Fpv;
    g_pilot_view = false;
    g_pilot_moved = false;
    g_pilot_key_hold = 0;
    fpv.fpv_set_view(libfpv_view());
    audio::start();
    fpv.fpv_overlay_set_action(kMoveNikoAction, "Move Niko here (Y)");

    fpv_input_status st{};
    st.struct_size = sizeof st;
    fpv.fpv_input_get_status(&st);
    fpvlog::write("drone launched at %.2f %.2f %.2f heading %.1f (ground %s), cam %u, input %d '%s'%s",
                  g_spawn_pos[0], g_spawn_pos[1], g_spawn_pos[2], heading, ground ? "hit" : "missed", g_cam,
                  int(st.source), st.device_name, st.needs_setup ? " (needs setup)" : "");
}

void stop(bool go_home = true) {
    fpv.fpv_overlay_set_action(kMoveNikoAction, nullptr);
    audio::stop();  // before the drone: the audio thread reads libfpv's synth
    if (g_cam) {
        script_camera(false);
        sh::call(N("DESTROY_CAM"), g_cam);
        g_cam = 0;
    }
    standin_end(go_home);
    pilot::hold(niko(), false);
    sh::call(N("SET_PLAYER_CONTROL"), g_player, true);
    if (g_pilot_view) fpv.fpv_audio_set_listener(nullptr, nullptr);
    g_pilot_view = false;
    g_shot.on = false;
    if (g_drone) {
        fpv.fpv_world_set_raycast_callback_ex(nullptr, nullptr);
        fpv.fpv_wind_set(nullptr);
        fpv.fpv_wind_set_sources(nullptr, 0);
        world::ignore_ped(0);
        fpv.fpv_drone_delete(g_drone);
        g_drone = nullptr;
    }
    fpv.fpv_set_view(FPV_VIEW_FPV);
    g_ending = Ending{};
    g_have_frame = false;
    {
        std::lock_guard<std::mutex> lock(g_osd_mutex);
        g_osd_visible = false;
        g_osd_no_signal = false;
        g_model_on = false;
    }
    fpvlog::write("drone stopped");
}

// Points the scripted camera from `from` at `at`, level (no roll).
void aim_camera(const float from[3], const float at[3], float fov) {
    float f[3] = {at[0] - from[0], at[1] - from[1], at[2] - from[2]};
    float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (len < 1e-3f) return;
    for (float &v : f) v /= len;
    float r[3] = {f[1], -f[0], 0};  // forward x world up
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1]);
    if (rl < 1e-3f) r[0] = 1, r[1] = 0, rl = 1;
    r[0] /= rl, r[1] /= rl;
    float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    float pitch, roll, heading;
    basis_to_euler(f, u, r, &pitch, &roll, &heading);
    sh::call(N("SET_CAM_POS"), g_cam, from[0], from[1], from[2]);
    sh::call(N("SET_CAM_ROT"), g_cam, pitch, roll, heading);
    sh::call(N("SET_CAM_FOV"), g_cam, fov);
    sh::call(N("SET_CAM_NEAR_CLIP"), g_cam, kNearClip);
    set_shot(from, f, u, r, fov);
}

// Cutscene: a camera behind and above where the drone came from, pulled in
// front of any wall in the way, watching the blast.
void place_blast_camera(const fpv_detonation &d) {
    float back[2] = {-d.velocity[0], -d.velocity[1]};
    float bl = std::sqrt(back[0] * back[0] + back[1] * back[1]);
    if (bl < 1.0f) {  // dropped straight down or hovered: behind the lens instead
        back[0] = -g_frame.camera.forward[0], back[1] = -g_frame.camera.forward[1];
        bl = std::sqrt(back[0] * back[0] + back[1] * back[1]);
    }
    if (bl < 1e-3f) back[0] = 0, back[1] = -1, bl = 1;
    back[0] /= bl, back[1] /= bl;
    const float *n = d.normal;
    bool has_normal = n[0] * n[0] + n[1] * n[1] + n[2] * n[2] > 0.5f;
    float origin[3];  // off the surface, so the probes start in open air
    for (int i = 0; i < 3; ++i) origin[i] = d.position[i] + (has_normal ? n[i] : (i == 2 ? 1.0f : 0.0f)) * 0.5f;
    const float len = std::sqrt(kCutsceneDistance * kCutsceneDistance + kCutsceneHeight * kCutsceneHeight);
    static const float kTurns[] = {0, 45, -45, 90, -90, 135, -135, 180};
    float best[3] = {0, 0, 0}, best_clear = -1;
    for (float turn : kTurns) {
        float a = turn / kRadToDeg, c = std::cos(a), sn = std::sin(a);
        float dir[3] = {(back[0] * c - back[1] * sn) * kCutsceneDistance / len,
                        (back[0] * sn + back[1] * c) * kCutsceneDistance / len, kCutsceneHeight / len};
        fpv_raycast_hit hit{};
        float clear = world::raycast(nullptr, origin, dir, len, &hit) ? hit.distance : len;
        if (clear > best_clear) {
            best_clear = clear;
            for (int i = 0; i < 3; ++i) best[i] = dir[i];
        }
        if (clear >= len * 0.9f) break;
    }
    float dist_out = best_clear * 0.85f < 2.5f ? 2.5f : best_clear * 0.85f;
    float from[3], at[3] = {d.position[0], d.position[1], d.position[2] + 1.0f};  // the fireball rises
    for (int i = 0; i < 3; ++i) from[i] = origin[i] + best[i] * dist_out;
    aim_camera(from, at, 50.0f);
    fpvlog::write("kamikaze: cutscene camera at %.2f %.2f %.2f (%.1f m clear)", from[0], from[1], from[2], best_clear);
}

bool signal_lost() { return g_ending.on && g_ending.choice.view == FPV_KAMIKAZE_VIEW_SIGNAL_LOST; }

[[maybe_unused]] void begin_ending(const fpv_detonation &d) {
    g_ending = Ending{};
    g_ending.on = true;
    kamikaze::Ending &e = g_ending.choice;
    e = kamikaze::ending();
    const Tuning &t = tuning();  // autotest overrides
    if (t.after_blast >= 0) e.view = t.after_blast == 1 ? FPV_KAMIKAZE_VIEW_CUTSCENE : FPV_KAMIKAZE_VIEW_SIGNAL_LOST;
    if (t.ending_then >= 0) e.then = fpv_kamikaze_ending_then(t.ending_then);
    if (t.ending_seconds >= 0) e.seconds = t.ending_seconds;
    // Nothing next and no view would leave the frozen wreck on screen: show the view.
    if (e.seconds <= 0 && e.then == FPV_KAMIKAZE_THEN_NOTHING) e.seconds = 1e9f;
    fpvlog::write("kamikaze: ending %s for %.1f s, then %s", e.seconds > 0 ? kamikaze::view_name(e.view) : "skipped",
                  e.seconds, kamikaze::then_name(e.then));
    if (e.seconds <= 0) return;  // the next tick does the "then"
    if (g_pilot_view) return;    // the player is on foot; F7 shows the goggles' lost signal
    if (e.view == FPV_KAMIKAZE_VIEW_CUTSCENE) {
        place_blast_camera(d);
    } else {
        g_view = View::Fpv;  // the goggles
        audio::set_static(true);
    }
    std::lock_guard<std::mutex> lock(g_osd_mutex);
    g_osd_visible = e.view == FPV_KAMIKAZE_VIEW_SIGNAL_LOST;
    g_osd_no_signal = g_osd_visible;
}

// Leaves the ending view: back to the goggles.
void end_ending() {
    if (!g_ending.on) return;
    if (signal_lost()) audio::set_static(false);
    g_ending = Ending{};
    g_view = View::Fpv;
    std::lock_guard<std::mutex> lock(g_osd_mutex);
    g_osd_no_signal = false;
}

// A new drone (R, or the ending's "respawn"): at the launch spot, or, after
// the pilot view, where Niko may have walked or driven off to, in front of him.
void respawn() {
    end_ending();
    if (g_standin.on) {  // Niko stays with the drone; its new spot must be streamed in
        sh::call(N("LOAD_SCENE"), g_spawn_pos[0], g_spawn_pos[1], g_spawn_pos[2]);
    }
    if (g_pilot_moved) {
        float pilot[3], heading;
        bool ground = place_spawn(pilot, &heading);
        fpvlog::write("drone respawned in front of Niko at %.2f %.2f %.2f (ground %s)", g_spawn_pos[0],
                      g_spawn_pos[1], g_spawn_pos[2], ground ? "hit" : "missed");
    }
    fpv.fpv_drone_repair(g_drone);
    fpv.fpv_drone_reset(g_drone, g_spawn_pos, g_spawn_rot);
    damage::reset();
}

// F: the view goes back to Niko, who walks and drives again; the drone stays
// where it is, armed or not, flown by the radio. Niko comes home from standing in.
void to_pilot() {
    standin_end(true);
    pilot::hold(niko(), false);
    g_pilot_view = true;
    g_pilot_moved = true;
    g_pilot_key_hold = 3;  // and until F is up: F also gets into cars
    g_shot.on = false;
    audio::set_static(false);
    script_camera(false);
    sh::call(N("SET_CAM_BEHIND_PED"), niko());
    {
        std::lock_guard<std::mutex> lock(g_osd_mutex);
        g_osd_visible = false;
    }
    fpvlog::write("pilot view: back to Niko, the drone left at %.2f %.2f %.2f%s", g_frame.position[0],
                  g_frame.position[1], g_frame.position[2], g_frame.flags & FPV_DRONE_ARMED ? " (armed)" : "");
}

// F7 from the pilot view: back to the drone, in the view the player had. A
// kamikaze blast's view still going on shows as the goggles' lost signal.
void back_to_drone() {
    g_pilot_view = false;
    fpv.fpv_audio_set_listener(nullptr, nullptr);
    sh::call(N("SET_PLAYER_CONTROL"), g_player, false);
    script_camera(true);
    if (g_ending.on) {
        g_ending.choice.view = FPV_KAMIKAZE_VIEW_SIGNAL_LOST;
        g_view = View::Fpv;
        audio::set_static(true);
        std::lock_guard<std::mutex> lock(g_osd_mutex);
        g_osd_visible = g_osd_no_signal = true;
    }
    fpvlog::write("pilot view over: back to the drone");
}

// The game camera as libfpv's listener in the pilot view, so the drone pans to
// where it is on screen.
void update_listener() {
    float pos[3], r[3], f[3], u[3], fov;
    if (!game_camera(pos, r, f, u, &fov)) return;
    float q[4];
    basis_to_quat(r, f, u, q);
    fpv.fpv_audio_set_listener(pos, q);
}

// The model pass's pose (render.cpp): the drone and the camera of this frame,
// hidden when something stands between them.
void update_model(const fpv_frame &f, float dt) {
    ModelPose m{};
    bool on = false;
    bool fpv_view = !g_pilot_view && g_view == View::Fpv && !(g_ending.on && !signal_lost());
    if (!fpv_view && !(f.flags & FPV_DRONE_DETONATED)) {
        float eye[3], er[3], ef[3], eu[3], fov;
        bool have_eye = false;
        if (g_pilot_view) {
            have_eye = game_camera(eye, er, ef, eu, &fov);
        } else if (g_shot.on) {
            for (int i = 0; i < 3; ++i)
                eye[i] = g_shot.pos[i], er[i] = g_shot.right[i], ef[i] = g_shot.forward[i], eu[i] = g_shot.up[i];
            fov = g_shot.fov;
            have_eye = true;
        }
        if (have_eye) {
            float to[3] = {f.position[0] - eye[0], f.position[1] - eye[1], f.position[2] - eye[2]};
            float d = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
            fpv_raycast_hit hit{};
            bool hidden = false;
            if (d > 0.3f) {
                for (float &v : to) v /= d;
                hidden = world::raycast(nullptr, eye, to, d - 0.25f, &hit) != 0;
            }
            static bool was_hidden = false;
            static int reports = 0;
            if (hidden != was_hidden && reports++ < 20) {
                was_hidden = hidden;
                fpvlog::write("model: %s (camera %.1f m away%s)", hidden ? "hidden" : "shown", d,
                              hidden ? ", something in between" : "");
                if (hidden)
                    fpvlog::write("model: ray hit at %.2f of %.2f m, object %08x", hit.distance, d, hit.object_id);
            }
            if (!hidden && d < 300.0f) {
                on = true;
                static const float kRight[3] = {1, 0, 0}, kUp[3] = {0, 0, 1}, kForward[3] = {0, 1, 0};
                rotate(f.rotation, kRight, m.right);
                rotate(f.rotation, kUp, m.up);
                rotate(f.rotation, kForward, m.forward);
                float arm = 0;
                for (int i = 0; i < 4; ++i) {
                    for (int k = 0; k < 3; ++k) m.motor[i][k] = f.motors[i].position[k];
                    m.rpm[i] = f.motors[i].rpm;
                    g_spin[i] = std::fmod(g_spin[i] + f.motors[i].rpm * dt * 6.2831853f / 60.0f * (i % 2 ? -1 : 1),
                                          6.2831853f);
                    m.spin[i] = g_spin[i];
                    m.prop_gone[i] = f.motors[i].prop_damage >= 1.0f;
                    arm += std::sqrt(m.motor[i][0] * m.motor[i][0] + m.motor[i][1] * m.motor[i][1]) / 4;
                }
                m.prop_radius = arm * 0.58f;  // a 5" prop on a 5" frame
                for (int i = 0; i < 3; ++i) {
                    m.pos[i] = f.position[i];
                    m.eye[i] = eye[i], m.eye_right[i] = er[i], m.eye_up[i] = eu[i], m.eye_forward[i] = ef[i];
                }
                m.fov_y = fov;
                int hour = 12, minute = 0;
                sh::call(N("GET_TIME_OF_DAY"), &hour, &minute);
                m.light = (hour >= 21 || hour < 5) ? 0.35f : (hour >= 19 || hour < 7) ? 0.65f : 1.0f;
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_osd_mutex);
    g_model = m;
    g_model_on = on;
}

}  // namespace

Tuning &tuning() {
    static Tuning t;
    return t;
}

// The game builds a camera as Rz(heading) Ry(roll) Rx(pitch) (measured with
// the autotest's cam test), so the bottom row of that matrix is
// (-sin roll, cos roll sin pitch, cos roll cos pitch) across right, forward, up.
void basis_to_euler(const float f[3], const float u[3], const float r[3], float *pitch, float *roll, float *heading) {
    float rz = r[2] < -1 ? -1 : r[2] > 1 ? 1 : r[2];
    *roll = std::asin(-rz) * kRadToDeg;
    if (std::fabs(rz) < 0.9999f) {
        *pitch = std::atan2(f[2], u[2]) * kRadToDeg;
        *heading = std::atan2(r[1], r[0]) * kRadToDeg;
    } else {  // knife edge: pitch and heading share an axis, put it all in heading
        *pitch = 0;
        *heading = std::atan2(-f[0], f[1]) * kRadToDeg;
    }
}

void inject_channels(const float channels[FPV_CHANNEL_COUNT]) {
    if (fpvapi::loaded()) fpv.fpv_input_set_channels(channels, FPV_CHANNEL_COUNT);
}

bool active() { return g_drone != nullptr; }

bool pilot_view() { return g_drone && g_pilot_view; }

// Niko faces the drone's heading.
void move_niko_here() {
    float pos[3] = {g_frame.position[0], g_frame.position[1], g_frame.position[2]};
    const float *f = g_frame.camera.forward;
    float heading = (f[0] * f[0] + f[1] * f[1] > 1e-6f) ? std::atan2(-f[0], f[1]) * kRadToDeg : 0;
    // Stand on whatever is under the drone (roof, road, floor); the world
    // probe already ignores Niko. Fall back to the drone's own height.
    float origin[3] = {pos[0], pos[1], pos[2] + 0.5f}, down[3] = {0, 0, -1};
    fpv_raycast_hit hit{};
    bool ground = world::raycast(nullptr, origin, down, 300.0f, &hit) != 0;
    float z = ground ? origin[2] - hit.distance : pos[2];
    stop(false);
    uint32_t ped = niko();
    sh::call(N("LOAD_SCENE"), pos[0], pos[1], z);  // near clips stream little
    if (sh::call<bool>(N("IS_CHAR_IN_ANY_CAR"), ped))
        sh::call(N("WARP_CHAR_FROM_CAR_TO_COORD"), ped, pos[0], pos[1], z + 1.0f);
    else
        sh::call(N("SET_CHAR_COORDINATES"), ped, pos[0], pos[1], z + 1.0f);
    sh::call(N("SET_CHAR_HEADING"), ped, heading);
    sh::call(N("SET_CAM_BEHIND_PED"), ped);
    fpvlog::write("moved Niko to the drone: %.2f %.2f %.2f (ground %s) heading %.1f", pos[0], pos[1], z,
                  ground ? "hit" : "missed", heading);
}

const fpv_frame &last_frame() { return g_frame; }

bool osd_frame(fpv_frame *out, bool *no_signal) {
    std::lock_guard<std::mutex> lock(g_osd_mutex);
    if (!g_osd_visible) return false;
    *out = g_osd_frame;
    *no_signal = g_osd_no_signal;
    return true;
}

bool model_pose(ModelPose *out) {
    std::lock_guard<std::mutex> lock(g_osd_mutex);
    if (!g_model_on) return false;
    *out = g_model;
    return true;
}

void toggle() {
    if (active() && g_pilot_view)
        back_to_drone();
    else if (active())
        stop();
    else
        start();
}

namespace {

// While the overlay is open on foot (no drone), Niko must
// not walk around as you type.
void hold_player_for_overlay() {
    static bool held = false;
    bool open = fpv.fpv_overlay_wants_input() != 0;
    if (g_drone || open == held) return;  // in flight tick() decides
    sh::call(N("SET_PLAYER_CONTROL"), player_index(), !open);
    held = open;
}

}  // namespace

void tick() {
    if (!fpvapi::loaded()) {
        static bool was = false, seen = false;  // the first reading is a baseline, not a press
        bool down = game_has_focus() && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (seen && down && !was) {  // F8 opens the overlay before the first flight too
            fpvlog::write("F8 pressed, loading libfpv");
            fpvapi::load();
        }
        was = down, seen = true;
        return;
    }
    kamikaze::init();
    forward_keys();
    hold_player_for_overlay();
    // Controls are read every tick (presses since the last read), drone or not.
    uint32_t camera_presses = fpvapi::presses(fpvapi::kControlCamera);
    uint32_t respawn_presses = fpvapi::presses(fpvapi::kControlRespawn);
    uint32_t pilot_presses = fpvapi::presses(fpvapi::kControlPilot);
    bool move_niko = fpvapi::presses(fpvapi::kControlMoveNiko) > 0;
    for (uint32_t id; (id = fpv.fpv_overlay_poll_action()) != 0;) move_niko |= id == kMoveNikoAction;
    if (!g_drone) return;

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = float(now.QuadPart - g_last_tick.QuadPart) / float(freq.QuadPart);
    g_last_tick = now;
    if (dt > 0.1f) dt = 0.1f;

    if (!g_pilot_view) hide_game_ui();  // the drone's views show the goggles or the drone, no game UI

    uint32_t ped = niko();
    if (sh::call<bool>(N("IS_CHAR_DEAD"), ped)) {  // flew the warhead into himself: the game takes over
        fpvlog::write("Niko died, drone stopped");
        stop(false);
        return;
    }
    // During the blast's view, R / F7 / Y act at once and replace the ending's "then".
    if (pilot_presses && !g_pilot_view) to_pilot();  // in the pilot view F is Niko's (cars)
    else if (camera_presses && !g_ending.on && !g_pilot_view)  // each press (or switch flip) steps once
        g_view = View((int(g_view) + int(camera_presses % 3)) % int(View::Count));
    if (respawn_presses) {
        if (g_ending.on) fpvlog::write("kamikaze: respawn (R) during the ending");
        respawn();
    }
    if (move_niko) {
        if (g_ending.on) fpvlog::write("kamikaze: Move Niko here during the ending");
        move_niko_here();
        return;
    }
    if (g_ending.on) {
        // The pause menu stops the game; the script thread may still tick.
        if (!sh::call<bool>(N("IS_PAUSE_MENU_ACTIVE"))) g_ending.elapsed += dt;
        const kamikaze::Ending &e = g_ending.choice;
        if (e.then != FPV_KAMIKAZE_THEN_NOTHING && g_ending.elapsed >= e.seconds) {
            fpvlog::write("kamikaze: ending over after %.1f s, %s", g_ending.elapsed, kamikaze::then_name(e.then));
            if (e.then == FPV_KAMIKAZE_THEN_RECALL) {
                stop();  // as F7: back to Niko
                return;
            }
            respawn();
        }
    }
    // Niko's controls: off while the drone's views show, his in the pilot view
    // (once F is up), off again while he types in the overlay.
    if (g_pilot_view) {
        if (g_pilot_key_hold > 0 && !key_down_scan(0x21 /* F */)) --g_pilot_key_hold;
        sh::call(N("SET_PLAYER_CONTROL"), g_player, g_pilot_key_hold <= 0 && !fpv.fpv_overlay_wants_input());
        update_listener();
    }
    static fpv_view reported_view = FPV_VIEW_FPV;
    if (libfpv_view() != reported_view) {
        reported_view = libfpv_view();
        fpv.fpv_set_view(reported_view);
    }

    float at[3];
    for (int i = 0; i < 3; ++i) at[i] = g_have_frame ? g_frame.position[i] : g_spawn_pos[i];
    uint32_t interior = 0;
    if (g_standin.on) sh::call(N("GET_INTERIOR_FROM_CHAR"), g_standin.niko, &interior);
    wind::update(dt, at, interior != 0);
    world::new_tick();

    fpv_frame frame{};
    frame.struct_size = sizeof frame;
    fpv_result r = fpv.fpv_drone_tick(g_drone, dt, &frame);
    if (r != FPV_OK) {
        static int reported = 0;
        if (reported++ < 5) fpvlog::write("fpv_drone_tick: %s", fpv.fpv_result_string(r));
        return;
    }
    // Until libfpv's sim thread has stepped the new drone, the frame shows a
    // stale snapshot (the world origin, or the last drone) and interpolates
    // from it: the camera would jump there and the stand-in would drag Niko
    // and the streamed world away from the spawn. libfpv's clock is not reset
    // per drone, so wait until it moved 50 ms past its first reading.
    if (g_first_sim_time < 0) g_first_sim_time = frame.sim_time;
    if (frame.sim_time < g_first_sim_time + 0.05) return;
    g_frame = frame;
    g_have_frame = true;
    standin_update(frame);
    update_pilot();
    update_pose(frame);
#if defined(LIBFPV_KAMIKAZE)
    if (fpvapi::kamikaze()) {
        fpv_detonation d;
        d.struct_size = sizeof d;
        while (fpv.fpv_kamikaze_poll_detonation(&d)) {
            kamikaze::explode(d);
            wind::blast(d.position, kWarheadBlastRadius);
            if (!g_ending.on) begin_ending(d);
        }
    }
#endif
    {
        std::lock_guard<std::mutex> lock(g_osd_mutex);
        g_osd_frame = frame;
        if (!g_ending.on) g_osd_visible = !g_pilot_view && g_view == View::Fpv;
    }
    if (!g_pilot_view && (!g_ending.on || signal_lost()))
        apply_camera(g_view == View::Fpv ? frame.camera : g_view == View::Chase ? frame.chase_camera : frame.los_camera,
                     g_view == View::Fpv);
    update_model(frame, dt);

    for (uint32_t i = 0; i < frame.impact_count && i < FPV_MAX_IMPACTS; ++i) {
        const fpv_impact &im = frame.impacts[i];
        if (im.speed < 1.0f) continue;  // resting contact
        fpvlog::write("impact at %.2f %.2f %.2f n %.2f %.2f %.2f speed %.1f impulse %.2f motor %d object %08x",
                      im.position[0], im.position[1], im.position[2], im.normal[0], im.normal[1], im.normal[2], im.speed,
                      im.impulse, int(im.motor_index), im.object_id);
    }
    effects::on_frame(frame);
    damage::update(g_drone, frame, dt);

    // Sparse trace for debugging the axes and the camera mapping.
    static float log_timer = 0;
    log_timer += dt;
    if (log_timer > 1.0f) {
        log_timer = 0;
        fpvlog::write("drone pos %.2f %.2f %.2f flags 0x%x alt %.2f thr %.2f fov %.1f near %.3f wind %.1f %.1f %.1f "
                      "view %d%s%s arm '%s'",
                      frame.position[0], frame.position[1], frame.position[2], frame.flags, frame.altitude,
                      frame.channels[2], g_fov, g_near_clip, frame.wind[0], frame.wind[1], frame.wind[2],
                      int(libfpv_view()), g_standin.on ? " stand-in" : "", g_pilot_view ? " pilot-view" : "",
                      frame.arming_disable_text);
    }
}

}  // namespace drone
