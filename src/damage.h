// What breaks a prop in Liberty City that libfpv can't see (libfpv
// INTEGRATION.md "Damage"): water (it also holds the drone back), fire,
// explosions, helicopter rotors, and whatever hurts Niko while he stands in
// for the drone. All through fpv_drone_damage_props, which does nothing while
// the player has damage off, so nothing here checks that setting.
#pragma once

#include <libfpv.h>

namespace damage {

// Every drone tick, with the frame just stepped. Script thread.
void update(fpv_drone *drone, const fpv_frame &frame, float dt);

// Health Niko lost while standing in for the drone: hits on the drone.
void stand_in_hit(fpv_drone *drone, const fpv_frame &frame, float health_lost);

// A new flight or a respawn.
void reset();

}  // namespace damage
