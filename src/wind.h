// Liberty City's weather and what blows near the drone, for libfpv's wind
// (libfpv INTEGRATION.md "Wind"): the weather's mean wind, helicopter rotors
// and explosions' blast waves.
#pragma once

#include <vector>

#include <libfpv.h>

namespace wind {

// A new flight: forget the explosions going on now.
void reset();

// Every drone tick, before fpv_drone_tick. enclosed: the drone is known to be
// in an interior (Niko stands in there). Script thread.
void update(float dt, const float drone_pos[3], bool enclosed);

// An explosion the mod made itself (the warhead): its blast wave.
void blast(const float pos[3], float radius);

// The helicopter rotors near the drone this tick (damage.cpp cuts props in them).
const std::vector<fpv_wind_source> &rotors();

}  // namespace wind
