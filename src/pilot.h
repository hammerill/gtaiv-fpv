// The pilot at the piloting spot (libfpv INTEGRATION.md "The pilot"): the
// FPV piloting pose on Niko while he stays home (stand-in off).
#pragma once

#include <cstdint>

namespace pilot {

// Plays the piloting pose on `ped` (on), or ends it (off). Idempotent, call
// it every tick with what is wanted: it starts the anim once it is loaded and
// starts it again if the game dropped it. Only one ped is posed at a time:
// posing another ends it on the first. Script thread.
void hold(uint32_t ped, bool on);

// Where the pose is on `ped` (0..1 of the loop), or -1 when it isn't playing.
float phase(uint32_t ped);

// Starts the pose on `ped` at `phase` (from phase() on another ped) at once,
// with no blend, so a hand-over between Niko and his copy doesn't show.
void hold_at(uint32_t ped, float phase);

// Forgets the posed ped without touching it (it was deleted).
void forget(uint32_t ped);

}  // namespace pilot
