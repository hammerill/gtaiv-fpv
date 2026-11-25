// Unattended test scenario, enabled by creating GTAIV/libfpv/autotest.txt
// and re-run whenever that file is touched. Moves the player to the nearest
// road, sweeps the world probe around him, then launches the drone and flies
// a fixed stick sequence, logging everything.
#include "autotest.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "drone.h"
#include "kamikaze.h"
#include "log.h"
#include "paths.h"
#include "scripthook.h"
#include "world.h"

namespace autotest {
namespace {

enum class Stage { Idle, Waiting, Settling, Flying, CamTest };
constexpr uint32_t kCarModel = 0x4b5c5320;  // joaat("admiral")
constexpr uint32_t kPedModel = 0xda0e92d1;  // joaat("m_m_business_02")
constexpr float kHoldHeight = 0.7f;         // low enough to hit a car or a ped's chest
uint32_t g_target_car = 0;
uint32_t g_target_ped = 0;
bool g_ped_mode = false;  // autotest.txt containing "ped" targets a pedestrian
bool g_cam_mode = false;  // "cam": camera rotation-order test instead of a flight

// autotest.txt: "car" or "ped" target, "cam" test, "still" (drone stays on the
// ground), "hover", "niko" (end with Move Niko here), and knobs fov=<deg>
// near=<m> osd=<0|1> secs=<flight seconds>, "far" (climb 35 m, then cruise),
// "incar" (Niko sits in the target car at launch), "osdoff" (OSD disable
// switch on: no OSD, no game text), kamikaze builds: "boom" (bomb arm switch
// on in flight, then into the target), "boomsw" (explode switch at 9 s
// instead), "signal" / "cutscene" (ending view), "respawn" / "recall" / "hold"
// (what follows it) and dur=<s> (its length), instead of the player's choice.
void read_config() {
    char buf[256] = {};
    if (FILE *f = std::fopen(paths::mod_file("autotest.txt").c_str(), "r")) {
        std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
    }
    g_ped_mode = std::strstr(buf, "ped") != nullptr;
    g_cam_mode = std::strstr(buf, "cam") != nullptr;
    drone::Tuning &t = drone::tuning();
    t = drone::Tuning{};
    t.armed_flight = std::strstr(buf, "still") == nullptr;
    t.hover = std::strstr(buf, "hover") != nullptr;
    t.move_niko = std::strstr(buf, "niko") != nullptr;
    t.long_range = std::strstr(buf, "far") != nullptr;
    t.in_car = std::strstr(buf, "incar") != nullptr;
    t.osd_off = std::strstr(buf, "osdoff") != nullptr;
    t.bomb = std::strstr(buf, "boom") != nullptr;
    t.explode_switch = std::strstr(buf, "boomsw") != nullptr;
    t.after_blast = std::strstr(buf, "cutscene") ? 1 : std::strstr(buf, "signal") ? 0 : -1;
    t.ending_then = std::strstr(buf, "respawn")  ? FPV_KAMIKAZE_THEN_RESPAWN
                    : std::strstr(buf, "recall") ? FPV_KAMIKAZE_THEN_RECALL
                    : std::strstr(buf, "hold")   ? FPV_KAMIKAZE_THEN_NOTHING
                                                 : -1;
    if (const char *p = std::strstr(buf, "dur=")) t.ending_seconds = float(std::atof(p + 4));
    if (const char *p = std::strstr(buf, "secs=")) t.seconds = float(std::atof(p + 5));
    if (const char *p = std::strstr(buf, "fov=")) t.fov = float(std::atof(p + 4));
    if (const char *p = std::strstr(buf, "near=")) t.near_clip = float(std::atof(p + 5));
    if (const char *p = std::strstr(buf, "osd=")) t.osd = std::atoi(p + 4) != 0;
    fpvlog::write("autotest: config '%s' -> fov %.1f near %.3f osd %d flight %d", buf, t.fov, t.near_clip, int(t.osd),
                  int(t.armed_flight));
}
Stage g_stage = Stage::Idle;
LARGE_INTEGER g_t0;
FILETIME g_trigger_time{};

// True when autotest.txt appeared or was touched since the last run.
bool triggered() {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(paths::mod_file("autotest.txt").c_str(), GetFileExInfoStandard, &data)) return false;
    if (CompareFileTime(&data.ftLastWriteTime, &g_trigger_time) == 0) return false;
    g_trigger_time = data.ftLastWriteTime;
    return true;
}

uint32_t player_ped() {
    uint32_t player = sh::call<uint32_t>(N("CONVERT_INT_TO_PLAYERINDEX"), sh::call<uint32_t>(N("GET_PLAYER_ID")));
    uint32_t ped = 0;
    sh::call(N("GET_PLAYER_CHAR"), player, &ped);
    return ped;
}

// Outdoors, so the flight has room: the nearest road node.
void move_to_road() {
    uint32_t ped = player_ped();
    float x, y, z, rx, ry, rz;
    sh::call(N("GET_CHAR_COORDINATES"), ped, &x, &y, &z);
    if (!sh::call<bool>(N("GET_CLOSEST_CAR_NODE"), x, y, z, &rx, &ry, &rz)) {
        fpvlog::write("autotest: no road node near %.2f %.2f %.2f, staying", x, y, z);
        return;
    }
    sh::call(N("SET_CHAR_COORDINATES"), ped, rx, ry, rz);
    sh::call(N("SET_CHAR_HEADING"), ped, 0.0f);  // north, along the test road (Broker, 585 1410)
    sh::call(N("LOAD_SCENE"), rx, ry, rz);
    fpvlog::write("autotest: moved player from %.2f %.2f %.2f to road %.2f %.2f %.2f", x, y, z, rx, ry, rz);
    sh::call(N("REQUEST_MODEL"), g_ped_mode ? kPedModel : kCarModel);
}

// A parked car or a standing pedestrian 16 m ahead, in the drone's path.
void spawn_target() {
    uint32_t model = g_ped_mode ? kPedModel : kCarModel;
    if (!sh::call<bool>(N("HAS_MODEL_LOADED"), model)) {
        fpvlog::write("autotest: target model not loaded, no target");
        return;
    }
    uint32_t ped = player_ped();
    float x, y, z, heading;
    sh::call(N("GET_CHAR_COORDINATES"), ped, &x, &y, &z);
    sh::call(N("GET_CHAR_HEADING"), ped, &heading);
    float h = heading / 57.2957795f;
    float tx = x - std::sin(h) * 16.0f, ty = y + std::cos(h) * 16.0f;
    if (g_ped_mode) {
        sh::call(N("CREATE_CHAR"), 4u, model, tx, ty, z, &g_target_ped, true);
        sh::call(N("SET_CHAR_HEADING"), g_target_ped, heading + 180.0f);
        uint32_t health = 0;
        sh::call(N("GET_CHAR_HEALTH"), g_target_ped, &health);
        fpvlog::write("autotest: target ped %u at %.2f %.2f %.2f health %u", g_target_ped, tx, ty, z, health);
    } else {
        sh::call(N("CREATE_CAR"), model, tx, ty, z, &g_target_car, true);
        sh::call(N("SET_CAR_HEADING"), g_target_car, heading + 90.0f);
        uint32_t health = 0;
        sh::call(N("GET_CAR_HEALTH"), g_target_car, &health);
        fpvlog::write("autotest: target car %u at %.2f %.2f %.2f health %u", g_target_car, tx, ty, z, health);
    }
    sh::call(N("MARK_MODEL_AS_NO_LONGER_NEEDED"), model);
}

void release_target() {
    if (g_target_car) {
        uint32_t health = 0;
        sh::call(N("GET_CAR_HEALTH"), g_target_car, &health);
        fpvlog::write("autotest: target car health after %u", health);
        sh::call(N("MARK_CAR_AS_NO_LONGER_NEEDED"), &g_target_car);
        g_target_car = 0;
    }
    if (g_target_ped) {
        uint32_t health = 0;
        sh::call(N("GET_CHAR_HEALTH"), g_target_ped, &health);
        fpvlog::write("autotest: target ped health after %u dead %d", health,
                      int(sh::call<bool>(N("IS_CHAR_DEAD"), g_target_ped)));
        sh::call(N("MARK_CHAR_AS_NO_LONGER_NEEDED"), &g_target_ped);
        g_target_ped = 0;
    }
}

float seconds_since(const LARGE_INTEGER &t) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return float(now.QuadPart - t.QuadPart) / float(freq.QuadPart);
}

void probe_sweep() {
    uint32_t ped = player_ped();
    float o[3];
    sh::call(N("GET_CHAR_COORDINATES"), ped, &o[0], &o[1], &o[2]);
    o[2] += 1.0f;
    world::ignore_ped(ped);  // the down ray should find the road, not Niko's head
    fpvlog::write("autotest: probe sweep from %.2f %.2f %.2f", o[0], o[1], o[2]);
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 0.785398f;
        float dir[3] = {std::cos(a), std::sin(a), 0};
        fpv_raycast_hit hit{};
        int r = world::raycast(nullptr, o, dir, 60.0f, &hit);
        fpvlog::write("autotest: ray %d dir %.2f %.2f -> %s d %.2f n %.2f %.2f %.2f", i, dir[0], dir[1],
                      r ? "hit" : "miss", hit.distance, hit.normal[0], hit.normal[1], hit.normal[2]);
    }
    float down[3] = {0, 0, -1};
    fpv_raycast_hit hit{};
    int r = world::raycast(nullptr, o, down, 100.0f, &hit);
    fpvlog::write("autotest: ray down -> %s d %.2f n %.2f %.2f %.2f", r ? "hit" : "miss", hit.distance, hit.normal[0],
                  hit.normal[1], hit.normal[2]);
}

float g_hold_z = 0;

// AETR + AUX1 arm, AUX2 angle mode, AUX3 beeper, AUX4 turtle, AUX5 air mode,
// AUX6 OSD disable, AUX7 bomb arm, AUX8 explode. A crude
// altitude hold keeps it kHoldHeight up while it cruises forward.
void sticks(float t) {
    const fpv_frame &f = drone::last_frame();
    const drone::Tuning &tu = drone::tuning();
    if (g_hold_z == 0 && t > 0.5f)  // after the first fresh frame
        g_hold_z = f.position[2] + (tu.long_range ? 35.0f : tu.hover ? 2.5f : kHoldHeight);
    // "far" climbs first; "boom" waits for the warhead to go live (6.5 s)
    const float cruise_at = tu.long_range ? 12.0f : tu.bomb ? 7.0f : 5.0f;
    float ch[12] = {0, 0, -1, 0, -1, 1, -1, -1, -1, tu.osd_off ? 1.0f : -1.0f, -1, -1};  // AUX7/8: kamikaze builds
    if (!drone::tuning().armed_flight) {  // parked: a still image for flicker checks
        drone::inject_channels(ch);
        return;
    }
    if (t > 2.0f) ch[4] = 1;  // arm
    if (t > 3.0f) {
        float err = g_hold_z - f.position[2];
        float tilt_comp = t > cruise_at && !tu.hover ? 0.06f : 0.0f;  // more thrust while pitched forward
        float thr = -0.71f + tilt_comp + 0.15f * err - 0.08f * f.velocity[2];  // -0.71 = hover
        ch[2] = thr < -0.9f ? -0.9f : thr > 0.3f ? 0.3f : thr;
    }
    if (t > cruise_at && !tu.hover) ch[1] = 0.5f;  // nose down: cruise forward
    if (tu.bomb && t > 3.5f) ch[10] = 1;            // warhead live 3 s later (OSD ARMING 3, STEADY)
    if (tu.explode_switch && t > 9.0f) ch[11] = 1;  // detonate in the air
    drone::inject_channels(ch);
}

// Camera rotation-order test. For each Euler input it projects the view axes
// predicted by every rotation order through the game's real camera; the order
// the game uses puts forward at the screen centre, up above it, right of it.
struct Euler { float pitch, roll, heading; };
const Euler kCamCases[] = {
    {0, 0, 0}, {0, 0, 90}, {30, 0, 0}, {0, 30, 0}, {0, 150, 0}, {0, 180, 30}, {40, 70, 0},
    {-50, 120, 60}, {80, 45, -30}, {20, -160, 135}, {60, 90, 45}, {-30, -100, 200},
};
constexpr int kOrders = 6;
uint32_t g_test_cam = 0;
int g_cam_case = 0, g_cam_wait = 0;
bool g_cam_pass2 = false;  // second pass sets each case through drone::basis_to_euler
float g_cam_pos[3];

void mat_mul(const float a[9], const float b[9], float o[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}

// Rx(pitch) Ry(roll) Rz(heading) multiplied in the order of `order`, then
// applied to the camera's rest axes (forward +Y, up +Z, right +X).
void predict(const Euler &e, int order, float f[3], float u[3], float r[3]) {
    float d = 1 / 57.2957795f, a, c, sn;
    float m[3][9];
    a = e.pitch * d, c = std::cos(a), sn = std::sin(a);
    float rx[9] = {1, 0, 0, 0, c, -sn, 0, sn, c};
    a = e.roll * d, c = std::cos(a), sn = std::sin(a);
    float ry[9] = {c, 0, sn, 0, 1, 0, -sn, 0, c};
    a = e.heading * d, c = std::cos(a), sn = std::sin(a);
    float rz[9] = {c, -sn, 0, sn, c, 0, 0, 0, 1};
    std::memcpy(m[0], rx, sizeof rx), std::memcpy(m[1], ry, sizeof ry), std::memcpy(m[2], rz, sizeof rz);
    static const int perms[kOrders][3] = {{2, 0, 1}, {2, 1, 0}, {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}};
    float t[9], M[9];
    mat_mul(m[perms[order][0]], m[perms[order][1]], t);
    mat_mul(t, m[perms[order][2]], M);
    for (int i = 0; i < 3; ++i) r[i] = M[i * 3], f[i] = M[i * 3 + 1], u[i] = M[i * 3 + 2];
}

void project(const float p[3], float *sx, float *sy) {
    uint32_t vp = 0;
    sh::call(N("GET_GAME_VIEWPORT_ID"), &vp);
    *sx = *sy = -1;
    sh::call(N("GET_VIEWPORT_POSITION_OF_COORD"), p[0], p[1], p[2], vp, sx, sy);
}

void cam_test_start() {
    uint32_t ped = player_ped();
    sh::call(N("GET_CHAR_COORDINATES"), ped, &g_cam_pos[0], &g_cam_pos[1], &g_cam_pos[2]);
    g_cam_pos[2] += 20.0f;
    sh::call(N("CREATE_CAM"), 14u, &g_test_cam);
    sh::call(N("SET_CAM_POS"), g_test_cam, g_cam_pos[0], g_cam_pos[1], g_cam_pos[2]);
    sh::call(N("SET_CAM_FOV"), g_test_cam, 60.0f);
    sh::call(N("SET_CAM_ACTIVE"), g_test_cam, true);
    sh::call(N("SET_CAM_PROPAGATE"), g_test_cam, true);
    sh::call(N("ACTIVATE_SCRIPTED_CAMS"), true, true);
    g_cam_case = 0;
    g_cam_wait = 0;
    g_cam_pass2 = false;
    fpvlog::write("autotest: cam test, %d cases, %d orders (0 = Rz Rx Ry, the current one)", int(sizeof kCamCases / sizeof kCamCases[0]), kOrders);
}

void cam_test_stop() {
    if (!g_test_cam) return;
    sh::call(N("ACTIVATE_SCRIPTED_CAMS"), false, false);
    sh::call(N("SET_CAM_ACTIVE"), g_test_cam, false);
    sh::call(N("DESTROY_CAM"), g_test_cam);
    g_test_cam = 0;
}

// True when finished.
bool cam_test_tick() {
    const int count = int(sizeof kCamCases / sizeof kCamCases[0]);
    if (g_cam_case >= count) {
        if (g_cam_pass2) return true;
        g_cam_pass2 = true;
        g_cam_case = 0;
    }
    const Euler &e = kCamCases[g_cam_case];
    if (g_cam_wait++ == 0) {
        if (g_cam_pass2) {  // the game's order is 1: does the conversion reproduce its basis?
            float f[3], u[3], r[3], p, ro, h;
            predict(e, 1, f, u, r);
            drone::basis_to_euler(f, u, r, &p, &ro, &h);
            sh::call(N("SET_CAM_ROT"), g_test_cam, p, ro, h);
        } else {
            sh::call(N("SET_CAM_ROT"), g_test_cam, e.pitch, e.roll, e.heading);
        }
        return false;
    }
    if (g_cam_wait < 6) return false;  // let a few frames render with it
    float rp, rr, rh;
    sh::call(N("GET_CAM_ROT"), g_test_cam, &rp, &rr, &rh);
    fpvlog::write("camtest %s case %d set %.0f %.0f %.0f read %.1f %.1f %.1f", g_cam_pass2 ? "conv" : "raw", g_cam_case, e.pitch, e.roll, e.heading, rp, rr,
                  rh);
    for (int o = 0; o < kOrders; ++o) {
        float f[3], u[3], r[3], s[3][2];
        predict(e, o, f, u, r);
        const float *axes[3] = {f, u, r};
        for (int k = 0; k < 3; ++k) {
            float p[3];
            for (int i = 0; i < 3; ++i) p[i] = g_cam_pos[i] + f[i] * 10.0f + (k ? axes[k][i] * 2.0f : 0.0f);
            project(p, &s[k][0], &s[k][1]);
        }
        // Back to the current conversion: does it reproduce this order's basis?
        float bp, br, bh;
        drone::basis_to_euler(f, u, r, &bp, &br, &bh);
        fpvlog::write("camtest %s case %d order %d fwd %.3f %.3f up %.3f %.3f right %.3f %.3f | conv %.1f %.1f %.1f",
                      g_cam_pass2 ? "conv" : "raw", g_cam_case, o, s[0][0], s[0][1], s[1][0], s[1][1], s[2][0], s[2][1], bp, br, bh);
    }
    ++g_cam_case;
    g_cam_wait = 0;
    return false;
}

}  // namespace

void tick() {
    static int poll = 0;
    if (g_stage == Stage::Idle) {
        if (++poll % 30 != 0 || !triggered()) return;
        g_stage = Stage::Waiting;
        read_config();
        QueryPerformanceCounter(&g_t0);
        fpvlog::write("autotest: triggered (%s), starting in 10 s", g_cam_mode ? "cam test" : g_ped_mode ? "ped" : "car");
        return;
    }
    float t = seconds_since(g_t0);
    switch (g_stage) {
        case Stage::Waiting:
            if (t < 10.0f || sh::call<bool>(N("IS_PAUSE_MENU_ACTIVE"))) return;
            if (drone::active()) drone::toggle();
            move_to_road();
            QueryPerformanceCounter(&g_t0);
            g_stage = Stage::Settling;
            return;
        case Stage::Settling:
            if (t < 3.0f) return;
            if (g_cam_mode) {
                cam_test_start();
                g_stage = Stage::CamTest;
                return;
            }
            probe_sweep();
            spawn_target();
            if (drone::tuning().in_car && g_target_car) sh::call(N("WARP_CHAR_INTO_CAR"), player_ped(), g_target_car);
            drone::toggle();
            if (!drone::active()) {
                g_stage = Stage::Idle;
                return;
            }
            fpvlog::write("autotest: flying");
            g_hold_z = 0;
            QueryPerformanceCounter(&g_t0);
            g_stage = Stage::Flying;
            return;
        case Stage::Flying:
            sticks(t);
            if (t > drone::tuning().seconds || !drone::active()) {
                if (drone::active() && drone::tuning().move_niko) {
                    drone::move_niko_here();
                    float x, y, z;
                    sh::call(N("GET_CHAR_COORDINATES"), player_ped(), &x, &y, &z);
                    fpvlog::write("autotest: Niko now at %.2f %.2f %.2f", x, y, z);
                }
                if (drone::active()) {
                    const float *p = drone::last_frame().position;
                    fpvlog::write("autotest: drone ended at %.2f %.2f %.2f", p[0], p[1], p[2]);
                    drone::toggle();
                    float x, y, z;
                    sh::call(N("GET_CHAR_COORDINATES"), player_ped(), &x, &y, &z);
                    fpvlog::write("autotest: Niko after F7 at %.2f %.2f %.2f in car %d", x, y, z,
                                  int(sh::call<bool>(N("IS_CHAR_IN_ANY_CAR"), player_ped())));
                }
                release_target();
                g_stage = Stage::Idle;
                drone::tuning() = drone::Tuning{};
                fpvlog::write("autotest: done");
            }
            return;
        case Stage::CamTest:
            if (cam_test_tick()) {
                cam_test_stop();
                g_stage = Stage::Idle;
                drone::tuning() = drone::Tuning{};
                fpvlog::write("autotest: done");
            }
            return;
        default:
            return;
    }
}

void abort() {
    if (g_stage == Stage::Idle) return;
    cam_test_stop();
    release_target();
    g_stage = Stage::Idle;
}

}  // namespace autotest
