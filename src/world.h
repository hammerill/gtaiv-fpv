// World collision for libfpv through the game's own physics probe.
#pragma once
#include <cstdint>
#include <vector>

#include <libfpv.h>

namespace world {

// Probe type flags (IV-SDK names): 2 static, 4 buildings, 8 vehicles, 64 ped
// collision, 128 objects.
constexpr uint32_t kTypeAll = 2 | 4 | 8 | 64 | 128;

enum class Kind : uint32_t { None = 0, Ped = 1, Vehicle = 2, Object = 3, Other = 4 };

// What a probe hit, as libfpv's fpv_impact.object_id: kind in the top 4 bits,
// the script handle below (0 for buildings and other non-script entities).
inline Kind kind_of(uint32_t object_id) { return Kind(object_id >> 28); }
inline uint32_t handle_of(uint32_t object_id) { return object_id & 0x0fffffffu; }

// libfpv material slots for what the probe hits (fpvapi.cpp sets their friction).
constexpr uint8_t kMaterialGround = 1;   // the map: asphalt, concrete, brick
constexpr uint8_t kMaterialVehicle = 2;  // a car's paint

// Makes probes pass through this ped (the player). 0 clears it. A second one
// (Niko's stand-in at the piloting spot) with ignore_second_ped.
void ignore_ped(uint32_t ped_handle);
void ignore_second_ped(uint32_t ped_handle);

// fpv_raycast_fn. Script thread only (it is called inside fpv_drone_tick).
int raycast(void *user, const float origin[3], const float dir[3], float max_distance, fpv_raycast_hit *hit);

// fpv_raycast_ex_fn: the same, and hits on vehicles, peds and objects carry
// their pose this frame, so a drone perched on a car rides along. Call
// new_tick() once before each fpv_drone_tick: every ray of a tick then gets
// the same pose for the same thing.
int raycast_ex(void *user, const float origin[3], const float dir[3], float max_distance, fpv_raycast_hit_ex *hit);
void new_tick();

// A ray of the previous tick hit a vehicle or ped within 3 m of where it
// started (the drone's own probes): something movable is next to the drone.
bool moving_thing_near();

// The CPed behind a ped's script handle, or null. For diagnostics.
void *ped_entity(uint32_t ped_handle);

// Script handles of every vehicle in the game's pool (empty if the pool
// couldn't be found). Script thread only.
std::vector<uint32_t> vehicles();

}  // namespace world
