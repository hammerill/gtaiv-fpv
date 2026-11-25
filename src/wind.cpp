#include "wind.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "fpvapi.h"
#include "log.h"
#include "scripthook.h"
#include "world.h"

namespace wind {
namespace {

using fpvapi::fpv;

// GET_CURRENT_WEATHER's values (ScriptHook SDK eWeather) as the mean wind
// 10 m up, m/s. The game only has a "windiness" in its weather types; these
// follow libfpv's guide (2-3 m/s a light breeze, 5-8 a breezy cloudy day,
// 10-12 a storm) and never go fully calm: real air never is, least of all
// over a harbour city.
constexpr float kWeatherMps[] = {
    1.5f,   // 0 extra sunny
    2.5f,   // 1 sunny
    8.0f,   // 2 sunny, windy
    5.0f,   // 3 cloudy
    7.0f,   // 4 raining
    4.5f,   // 5 drizzle
    1.5f,   // 6 foggy
    11.0f,  // 7 lightning (the storm)
    1.5f,   // 8 extra sunny 2
    8.0f,   // 9 sunny, windy 2
};
// GTA IV has no wind direction for scripts. The prevailing wind over New
// York blows from the south-west: towards the north-east here (+X east, +Y north).
constexpr float kDir[2] = {0.7071f, 0.7071f};
constexpr float kSpeedSeconds = 20.0f;   // a weather change blends in over this
constexpr float kSourceRange = 150.0f;   // m: helicopters farther away can't reach the drone
constexpr float kHeliMass = 2500.0f;     // kg, a Maverick-sized helicopter; the game doesn't tell
constexpr float kAirDensity = 1.2f;
constexpr float kBlastWatch = 60.0f;     // m: explosions this close blow the drone
constexpr float kBlastRadius = 4.0f;     // m: a grenade or a car; the game doesn't tell which

float g_speed = -1;
bool g_explosion_near = false;
float g_quiet = 0;          // s since no explosion was near (to see the next one as new)
float g_own_blast = 0;      // s left in which a near explosion is the warhead's
float g_log_timer = 0;
std::vector<fpv_wind_source> g_rotors;

bool explosion_in(const float c[3], float r) {
    return sh::call<bool>(N("IS_EXPLOSION_IN_SPHERE"), -1, c[0], c[1], c[2], r);
}

// The game only says whether an explosion is inside a sphere. Find how far
// (the smallest of a few spheres around the drone) and which way (spheres
// around it) it went off, for the blast's centre.
void locate_and_blast(const float at[3]) {
    float r = kBlastWatch;
    for (float t : {3.0f, 6.0f, 12.0f, 24.0f, 48.0f})
        if (explosion_in(at, t)) {
            r = t;
            break;
        }
    static const float kDirs[10][3] = {{1, 0, 0},       {-1, 0, 0},      {0, 1, 0},        {0, -1, 0},
                                       {0.7f, 0.7f, 0}, {-0.7f, 0.7f, 0}, {0.7f, -0.7f, 0}, {-0.7f, -0.7f, 0},
                                       {0, 0, 1},       {0, 0, -1}};
    float sum[3] = {0, 0, 0};
    int hits = 0;
    for (const auto &d : kDirs) {
        float c[3] = {at[0] + d[0] * r * 0.75f, at[1] + d[1] * r * 0.75f, at[2] + d[2] * r * 0.75f};
        if (!explosion_in(c, r * 0.5f)) continue;
        for (int i = 0; i < 3; ++i) sum[i] += d[i];
        ++hits;
    }
    float pos[3] = {at[0], at[1], at[2]};
    float l = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
    if (hits && l > 1e-3f)
        for (int i = 0; i < 3; ++i) pos[i] += sum[i] / l * r * 0.75f;
    fpv.fpv_wind_add_blast(pos, kBlastRadius);
    fpvlog::write("wind: blast of an explosion within %.0f m of the drone (%d of 10 directions)", r, hits);
}

void blasts(float dt, const float at[3]) {
    g_own_blast = std::max(0.0f, g_own_blast - dt);
    bool close = explosion_in(at, kBlastWatch);
    if (!close) {
        g_quiet += dt;
        if (g_quiet > 1.0f) g_explosion_near = false;  // the next one is a new one
        return;
    }
    g_quiet = 0;
    if (g_explosion_near) return;
    g_explosion_near = true;
    if (g_own_blast > 0) return;  // the warhead's own, already blown
    locate_and_blast(at);
}

// Helicopters with someone at the controls: rotor downwash, from momentum
// theory with the helicopter's weight as the thrust.
void helicopters(const float at[3]) {
    g_rotors.clear();
    for (uint32_t car : world::vehicles()) {
        if (g_rotors.size() >= FPV_MAX_WIND_SOURCES) break;
        if (!sh::call<bool>(N("DOES_VEHICLE_EXIST"), car)) continue;
        uint32_t model = 0;
        sh::call(N("GET_CAR_MODEL"), car, &model);
        if (!sh::call<bool>(N("IS_THIS_MODEL_A_HELI"), model)) continue;
        float p[3];
        sh::call(N("GET_CAR_COORDINATES"), car, &p[0], &p[1], &p[2]);
        float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
        if (dx * dx + dy * dy + dz * dz > kSourceRange * kSourceRange) continue;
        uint32_t driver = 0;
        sh::call(N("GET_DRIVER_OF_CAR"), car, &driver);
        if (!driver || sh::call<bool>(N("IS_CAR_DEAD"), car)) continue;  // a parked one's rotor is still
        float mn[4] = {}, mx[4] = {};  // Vector3 out-params, with room to spare
        sh::call(N("GET_MODEL_DIMENSIONS"), model, mn, mx);
        float length = std::max(mx[1] - mn[1], mx[0] - mn[0]);
        if (!(length > 1 && length < 40)) length = 12;
        float top = mx[2] > 0.5f && mx[2] < 10 ? mx[2] : 2.0f;
        fpv_wind_source s{};
        float radius = std::clamp(0.42f * length, 0.3f, 8.0f);  // the main rotor spans about the fuselage
        s.position[0] = p[0], s.position[1] = p[1], s.position[2] = p[2] + top;
        s.direction[2] = -1;
        s.radius = radius;
        s.speed = std::sqrt(kHeliMass * 9.81f / (2.0f * kAirDensity * 3.14159265f * radius * radius));
        s.type = FPV_WIND_SOURCE_ROTOR;
        g_rotors.push_back(s);
    }
}

}  // namespace

void reset() {
    g_speed = -1;
    g_explosion_near = true;  // whatever burns now is history
    g_quiet = 0;
    g_own_blast = 0;
    g_log_timer = 0;
    g_rotors.clear();
}

void blast(const float pos[3], float radius) {
    fpv.fpv_wind_add_blast(pos, radius);
    g_own_blast = 2.0f;
}

const std::vector<fpv_wind_source> &rotors() { return g_rotors; }

void update(float dt, const float at[3], bool enclosed) {
    uint32_t weather = 0;
    sh::call(N("GET_CURRENT_WEATHER"), &weather);
    float target = weather < sizeof kWeatherMps / sizeof kWeatherMps[0] ? kWeatherMps[weather] : 4.0f;
    g_speed = g_speed < 0 ? target : g_speed + (target - g_speed) * (1.0f - std::exp(-dt / kSpeedSeconds));

    fpv_wind w;
    fpv.fpv_wind_defaults(&w);
    w.velocity[0] = kDir[0] * g_speed;
    w.velocity[1] = kDir[1] * g_speed;
    if (enclosed) w.flags |= FPV_WIND_ENCLOSED;
    fpv.fpv_wind_set(&w);

    helicopters(at);
    fpv.fpv_wind_set_sources(g_rotors.data(), uint32_t(g_rotors.size()));
    blasts(dt, at);

    g_log_timer -= dt;
    if (g_log_timer <= 0) {
        g_log_timer = 10.0f;
        fpvlog::write("wind: weather %u, %.1f m/s towards the north-east%s, %u rotor(s) near", weather, g_speed,
                      enclosed ? ", indoors" : "", unsigned(g_rotors.size()));
    }
}

}  // namespace wind
