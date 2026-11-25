#include "damage.h"

#include <cmath>
#include <cstdlib>

#include "fpvapi.h"
#include "log.h"
#include "scripthook.h"
#include "wind.h"

namespace damage {
namespace {

using fpvapi::fpv;

constexpr float kWaterDrag = 6.0f;        // 1/s: a dead quad sinks at ~1.6 m/s, full throttle climbs out
constexpr float kWaterPropRate = 1.0f;    // a whole prop in a second under water
constexpr float kFirePropRate = 0.5f;     // all four, per second in flames
constexpr float kFireRange = 1.5f;        // m from the drone's centre
constexpr float kExplosionRange = 4.0f;   // m: inside a grenade's or a car's fireball
constexpr float kRotorSlab = 0.4f;        // m above or below a turning rotor's disc

bool g_in_water = false;
bool g_in_fire = false;
bool g_explosion = true;  // one already going on is not a new hit
float g_rotor_cooldown = 0;

void rotate(const float q[4], const float v[3], float out[3]) {
    float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
          tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// GET_WATER_HEIGHT(x, y, z, &height): the sea, the rivers and the lakes are
// one water system in GTA IV. False where there is no water at x, y.
bool water_height(float x, float y, float z, float *height) {
    return sh::call<bool>(N("GET_WATER_HEIGHT"), x, y, z, height);
}

// Water: the drone goes in (it isn't in the collision), slows down, and the
// props under the surface break.
void water(fpv_drone *drone, const fpv_frame &f, float dt) {
    float surface = 0;
    bool any = water_height(f.position[0], f.position[1], f.position[2], &surface);
    bool centre_in = any && f.position[2] < surface;
    if (centre_in) {
        float k = 1.0f - std::exp(-kWaterDrag * dt);
        float dv[3] = {-f.velocity[0] * k, -f.velocity[1] * k, -f.velocity[2] * k};
        fpv.fpv_drone_push(drone, dv, 0);
    }
    int props_in = 0;
    for (int i = 0; i < 4; ++i) {
        float hub[3];
        rotate(f.rotation, f.motors[i].position, hub);
        for (int k = 0; k < 3; ++k) hub[k] += f.position[k];
        float z = 0;
        if (water_height(hub[0], hub[1], hub[2], &z) && hub[2] < z) {
            fpv.fpv_drone_damage_props(drone, i, kWaterPropRate * dt);
            ++props_in;
        }
    }
    bool in = centre_in || props_in > 0;
    if (in != g_in_water) {
        g_in_water = in;
        if (in)
            fpvlog::write("damage: into the water at %.2f %.2f %.2f, surface %.2f (GET_WATER_HEIGHT), %d prop(s) under",
                          f.position[0], f.position[1], f.position[2], surface, props_in);
        else
            fpvlog::write("damage: out of the water");
    }
}

void fire(fpv_drone *drone, const fpv_frame &f, float dt) {
    uint32_t fires = sh::call<uint32_t>(N("GET_NUMBER_OF_FIRES_IN_RANGE"), f.position[0], f.position[1],
                                        f.position[2], kFireRange);
    bool in = fires > 0;
    if (in) fpv.fpv_drone_damage_props(drone, -1, kFirePropRate * dt);
    if (in != g_in_fire) {
        g_in_fire = in;
        fpvlog::write("damage: %s fire", in ? "in" : "out of the");
    }
}

void explosions(fpv_drone *drone, const fpv_frame &f) {
    bool in = sh::call<bool>(N("IS_EXPLOSION_IN_SPHERE"), -1, f.position[0], f.position[1], f.position[2],
                             kExplosionRange);
    if (in && !g_explosion) {
        fpv.fpv_drone_damage_props(drone, -1, 1.0f);
        fpvlog::write("damage: explosion within %.0f m: all props", kExplosionRange);
    }
    g_explosion = in;
}

// A turning rotor's disc cuts the props.
void rotors(fpv_drone *drone, const fpv_frame &f, float dt) {
    g_rotor_cooldown -= dt;
    if (g_rotor_cooldown > 0) return;
    for (const fpv_wind_source &r : wind::rotors()) {
        float dx = f.position[0] - r.position[0], dy = f.position[1] - r.position[1];
        float dz = f.position[2] - r.position[2];
        if (std::fabs(dz) > kRotorSlab || dx * dx + dy * dy > r.radius * r.radius) continue;
        fpv.fpv_drone_damage_props(drone, -1, 1.0f);
        g_rotor_cooldown = 1.0f;
        fpvlog::write("damage: into a helicopter's rotor: all props");
        return;
    }
}

}  // namespace

void reset() {
    g_in_water = g_in_fire = false;
    g_explosion = true;
    g_rotor_cooldown = 0;
}

void update(fpv_drone *drone, const fpv_frame &f, float dt) {
    if (!drone || dt <= 0) return;
    water(drone, f, dt);
    fire(drone, f, dt);
    explosions(drone, f);
    rotors(drone, f, dt);
}

// Niko at the drone takes the bullets, punches and blasts aimed at it. About
// a pistol round (25 health) breaks most of one prop; a blast (100 or more)
// all four.
void stand_in_hit(fpv_drone *drone, const fpv_frame &f, float lost) {
    if (!drone || lost <= 0) return;
    int motor = lost >= 100 ? -1 : std::rand() % 4;
    float amount = motor < 0 ? 1.0f : std::fmin(1.0f, lost / 30.0f);
    fpv.fpv_drone_damage_props(drone, motor, amount);
    static int reported = 0;
    if (reported++ < 20)
        fpvlog::write("damage: Niko standing in lost %.0f health: %.2f on %s", lost, amount,
                      motor < 0 ? "all props" : "one prop");
}

}  // namespace damage
