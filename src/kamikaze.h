// Kamikaze builds of libfpv (warhead, bomb arm and explode switches): the
// game's side of a detonation. Only in mod builds with GTAIV_FPV_KAMIKAZE
// (LIBFPV_KAMIKAZE); otherwise everything here is a no-op.
#pragma once

#include <cstdint>

#include <libfpv.h>

#if !defined(LIBFPV_KAMIKAZE)
// Plain builds: just enough of libfpv's kamikaze types for the shared code
// to compile. Nothing asks libfpv for them, and no detonation ever comes.
typedef enum fpv_kamikaze_ending_view { FPV_KAMIKAZE_VIEW_SIGNAL_LOST = 0, FPV_KAMIKAZE_VIEW_CUTSCENE = 1 } fpv_kamikaze_ending_view;
typedef enum fpv_kamikaze_ending_then {
    FPV_KAMIKAZE_THEN_RESPAWN = 0,
    FPV_KAMIKAZE_THEN_RECALL = 1,
    FPV_KAMIKAZE_THEN_NOTHING = 2
} fpv_kamikaze_ending_then;
typedef struct fpv_detonation {
    uint32_t struct_size;
    double sim_time;
    uint32_t cause;
    float position[3], velocity[3], normal[3];
    uint8_t material;
    uint32_t object_id;
    int32_t motor_index;
} fpv_detonation;
#define FPV_DRONE_DETONATED 0u
#endif

namespace kamikaze {

// What happens after the blast. The player picks it in libfpv's overlay
// (F8 > Kamikaze), libfpv saves it; the mod reads it at the detonation.
struct Ending {
    fpv_kamikaze_ending_view view = FPV_KAMIKAZE_VIEW_SIGNAL_LOST;
    float seconds = 2.0f;  // how long the view shows; 0 = skip it (libfpv's default 2 s)
    fpv_kamikaze_ending_then then = FPV_KAMIKAZE_THEN_RESPAWN;
};

// The player's current choice (defaults with an older libfpv). Script thread.
Ending ending();

// Once libfpv is loaded: tells libfpv which endings GTA IV supports. Script thread.
void init();

// The game's explosion at the detonation point. Script thread.
void explode(const fpv_detonation &d);

const char *view_name(fpv_kamikaze_ending_view v);
const char *then_name(fpv_kamikaze_ending_then t);

}  // namespace kamikaze
