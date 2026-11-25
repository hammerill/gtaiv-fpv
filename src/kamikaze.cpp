#include "kamikaze.h"

#include <cmath>

#include "fpvapi.h"
#include "log.h"
#include "scripthook.h"

namespace kamikaze {
namespace {

using fpvapi::fpv;

// Older versions of the mod had an "After the blast" F8 > Fly button with
// this id; libfpv's Kamikaze tab holds the setting now.
constexpr uint32_t kOldEndingAction = 2;

// A 5" strike quad carries a few hundred grams of explosive: grenade-sized.
constexpr uint32_t kExplosionGrenade = 0;
constexpr float kExplosionRadius = 5.0f;
constexpr float kCamShake = 1.0f;

bool g_ready = false;

}  // namespace

const char *view_name(fpv_kamikaze_ending_view v) {
    return v == FPV_KAMIKAZE_VIEW_CUTSCENE ? "cutscene" : "signal lost";
}

const char *then_name(fpv_kamikaze_ending_then t) {
    return t == FPV_KAMIKAZE_THEN_RECALL ? "recall" : t == FPV_KAMIKAZE_THEN_NOTHING ? "nothing" : "respawn";
}

Ending ending() {
    Ending e;
    if (!fpvapi::kamikaze()) return e;
#if defined(LIBFPV_KAMIKAZE)
    // An older libfpv copies only the fields it knows: the rest keep the defaults.
    fpv_kamikaze_config kc{};
    kc.struct_size = sizeof kc;
    kc.ending_duration = e.seconds;
    kc.ending_view = e.view;
    kc.ending_then = e.then;
    if (fpv.fpv_kamikaze_get_config(&kc) != FPV_OK) return e;
    e.view = kc.ending_view == FPV_KAMIKAZE_VIEW_CUTSCENE ? FPV_KAMIKAZE_VIEW_CUTSCENE : FPV_KAMIKAZE_VIEW_SIGNAL_LOST;
    e.seconds = !(kc.ending_duration > 0) ? 0 : kc.ending_duration > 30 ? 30 : kc.ending_duration;
    e.then = kc.ending_then == FPV_KAMIKAZE_THEN_RECALL    ? FPV_KAMIKAZE_THEN_RECALL
             : kc.ending_then == FPV_KAMIKAZE_THEN_NOTHING ? FPV_KAMIKAZE_THEN_NOTHING
                                                           : FPV_KAMIKAZE_THEN_RESPAWN;
#endif
    return e;
}

void init() {
    if (g_ready || !fpvapi::kamikaze()) return;
    g_ready = true;
    fpv.fpv_overlay_set_action(kOldEndingAction, nullptr);  // left by an older core before a hot reload
#if defined(LIBFPV_KAMIKAZE)
    // GTA IV does every ending: Niko is there to recall to.
    if (fpv.fpv_kamikaze_set_ending_support) {
        uint32_t views = (1u << FPV_KAMIKAZE_VIEW_SIGNAL_LOST) | (1u << FPV_KAMIKAZE_VIEW_CUTSCENE);
        uint32_t thens = (1u << FPV_KAMIKAZE_THEN_RESPAWN) | (1u << FPV_KAMIKAZE_THEN_RECALL) |
                         (1u << FPV_KAMIKAZE_THEN_NOTHING);
        fpv_result r = fpv.fpv_kamikaze_set_ending_support(views, thens, FPV_KAMIKAZE_VIEW_SIGNAL_LOST,
                                                           FPV_KAMIKAZE_THEN_RESPAWN);
        if (r != FPV_OK) fpvlog::write("fpv_kamikaze_set_ending_support: %s", fpv.fpv_result_string(r));
    } else {
        fpvlog::write("kamikaze: libfpv.dll has no ending choice, using signal lost / 2 s / respawn");
    }
#endif
    Ending e = ending();
    fpvlog::write("kamikaze: after the blast: %s for %.1f s, then %s", view_name(e.view), e.seconds,
                  then_name(e.then));
}

void explode(const fpv_detonation &d) {
    static const char *causes[] = {"impact", "explode switch", "game"};
    sh::call(N("ADD_EXPLOSION"), d.position[0], d.position[1], d.position[2], kExplosionGrenade, kExplosionRadius,
             true, false, kCamShake);
    fpvlog::write("kamikaze: detonation (%s) at %.2f %.2f %.2f speed %.1f object %08x",
                  causes[unsigned(d.cause) < 3 ? d.cause : 2], d.position[0], d.position[1], d.position[2],
                  std::sqrt(d.velocity[0] * d.velocity[0] + d.velocity[1] * d.velocity[1] +
                             d.velocity[2] * d.velocity[2]),
                  d.object_id);
}

}  // namespace kamikaze
