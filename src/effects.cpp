#include "effects.h"

#include <windows.h>

#include <cmath>

#include "log.h"
#include "scripthook.h"
#include "world.h"

namespace effects {
namespace {

// A 5" quad is ~0.65 kg; at 30 m/s that is a real hit for a pedestrian and a
// dent for a car. Speeds are the relative normal speed libfpv reports (m/s).
constexpr float kPedRagdollSpeed = 4.0f;
constexpr float kPedDamagePerMs = 2.0f;    // health points per m/s (peds have ~200)
constexpr float kCarDamageSpeed = 5.0f;
constexpr float kCarDamagePerMs = 3.0f;    // body health points per m/s (cars have 1000)
constexpr float kDroneMass = 0.65f;
constexpr float kPushGain = 1.0f;          // APPLY_FORCE_* scale on the drone's momentum
constexpr DWORD kCooldownMs = 500;         // one effect per entity per half second

struct Recent {
    uint32_t object_id;
    DWORD time;
};
Recent g_recent[16];

bool cooling_down(uint32_t object_id) {
    DWORD now = GetTickCount();
    for (const Recent &r : g_recent)
        if (r.object_id == object_id && now - r.time < kCooldownMs) return true;
    return false;
}

void start_cooldown(uint32_t object_id) {
    Recent *oldest = &g_recent[0];
    for (Recent &r : g_recent)
        if (r.time < oldest->time) oldest = &r;
    *oldest = {object_id, GetTickCount()};
}

uint32_t player_ped() {
    uint32_t player = sh::call<uint32_t>(N("CONVERT_INT_TO_PLAYERINDEX"), sh::call<uint32_t>(N("GET_PLAYER_ID")));
    uint32_t ped = 0;
    sh::call(N("GET_PLAYER_CHAR"), player, &ped);
    return ped;
}

void report_gone(const char *what, uint32_t handle, float speed) {
    static int reported = 0;
    if (reported++ < 30) fpvlog::write("effects: hit %s %u at %.1f m/s, but it no longer exists", what, handle, speed);
}

bool hit_ped(uint32_t ped, float speed, const float push[3]) {
    if (ped == player_ped()) return false;
    if (!sh::call<bool>(N("DOES_CHAR_EXIST"), ped)) return report_gone("ped", ped, speed), false;
    if (speed < kPedRagdollSpeed) return false;
    sh::call(N("SWITCH_PED_TO_RAGDOLL"), ped, 1000u, 3000u, 0u, 1u, 1u, 0u);
    sh::call(N("DAMAGE_CHAR"), ped, uint32_t(speed * kPedDamagePerMs), false);
    sh::call(N("APPLY_FORCE_TO_PED"), ped, 3u, push[0], push[1], push[2], 0.0f, 0.0f, 0.0f, 0u, 1u, 1u, 1u);
    uint32_t health = 0;
    sh::call(N("GET_CHAR_HEALTH"), ped, &health);
    fpvlog::write("effects: ped %u hit at %.1f m/s -> ragdoll %d health %u", ped, speed,
                  int(sh::call<bool>(N("IS_PED_RAGDOLL"), ped)), health);
    return true;
}

bool hit_vehicle(uint32_t car, float speed, const float push[3]) {
    if (!sh::call<bool>(N("DOES_VEHICLE_EXIST"), car)) return report_gone("car", car, speed), false;
    if (speed < kCarDamageSpeed) return false;
    sh::call(N("APPLY_FORCE_TO_CAR"), car, 3u, push[0], push[1], push[2], 0.0f, 0.0f, 0.0f, 0u, 1u, 1u, 1u);
    uint32_t health = 0;
    sh::call(N("GET_CAR_HEALTH"), car, &health);
    uint32_t damage = uint32_t(speed * kCarDamagePerMs);
    uint32_t left = health > damage ? health - damage : 0;
    sh::call(N("SET_CAR_HEALTH"), car, left);
    fpvlog::write("effects: car %u hit at %.1f m/s -> health %u -> %u", car, speed, health, left);
    return true;
}

bool hit_object(uint32_t object, float speed, const float push[3]) {
    if (!sh::call<bool>(N("DOES_OBJECT_EXIST"), object)) return false;
    sh::call(N("APPLY_FORCE_TO_OBJECT"), object, 3u, push[0], push[1], push[2], 0.0f, 0.0f, 0.0f, 0u, 1u, 1u, 1u);
    fpvlog::write("effects: object %u hit at %.1f m/s", object, speed);
    return true;
}

}  // namespace

void on_frame(const fpv_frame &frame) {
    for (uint32_t i = 0; i < frame.impact_count && i < FPV_MAX_IMPACTS; ++i) {
        const fpv_impact &im = frame.impacts[i];
        world::Kind kind = world::kind_of(im.object_id);
        uint32_t handle = world::handle_of(im.object_id);
        if (!handle || im.speed < 1.0f || cooling_down(im.object_id)) continue;
        // Push along the drone's travel, sized by its momentum.
        float push[3];
        for (int k = 0; k < 3; ++k) push[k] = frame.velocity[k] * kDroneMass * kPushGain;
        bool applied = false;
        switch (kind) {
            case world::Kind::Ped: applied = hit_ped(handle, im.speed, push); break;
            case world::Kind::Vehicle: applied = hit_vehicle(handle, im.speed, push); break;
            case world::Kind::Object: applied = hit_object(handle, im.speed, push); break;
            default: break;
        }
        if (applied) start_cooldown(im.object_id);
    }
}

}  // namespace effects
