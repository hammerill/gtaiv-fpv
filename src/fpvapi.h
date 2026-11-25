// libfpv.dll, loaded at runtime on the first launch so a missing or broken DLL
// never stops the game (or the rest of the mod) from starting.
#pragma once

#include <libfpv.h>

namespace fpvapi {

// Everything the mod uses from libfpv 1.1 (the release ZIP ships the two
// together). A libfpv.dll without one of these isn't loaded.
#define FPV_FUNCTIONS(X)                                                                         \
    X(fpv_global_config_defaults) X(fpv_global_init) X(fpv_last_error) X(fpv_result_string)      \
    X(fpv_version_string) X(fpv_shared_data_dir) X(fpv_material_set)                             \
    X(fpv_world_set_raycast_callback_ex) X(fpv_drone_desc_defaults)                              \
    X(fpv_drone_create) X(fpv_drone_delete) X(fpv_drone_reset) X(fpv_drone_repair)               \
    X(fpv_drone_tick) X(fpv_drone_damage_props) X(fpv_drone_set_pilot_position) X(fpv_drone_push) \
    X(fpv_input_key) X(fpv_input_get_status) X(fpv_input_set_channels)                           \
    X(fpv_texture_get) X(fpv_overlay_is_open) X(fpv_overlay_wants_input) X(fpv_overlay_set_display) \
    X(fpv_overlay_mouse_move) X(fpv_overlay_mouse_button) X(fpv_overlay_text)                    \
    X(fpv_overlay_render) X(fpv_osd_draw_data) X(fpv_audio_configure) X(fpv_audio_read)          \
    X(fpv_audio_set_listener) X(fpv_set_view)                                                    \
    X(fpv_overlay_set_action) X(fpv_overlay_poll_action) X(fpv_gps_set_home)                     \
    X(fpv_pilot_set_support) X(fpv_pilot_get_config)                                             \
    X(fpv_wind_defaults) X(fpv_wind_set) X(fpv_wind_set_sources) X(fpv_wind_add_blast)           \
    X(fpv_controls_set) X(fpv_control_poll)

// Only in mod builds for a kamikaze libfpv (GTAIV_FPV_KAMIKAZE, which
// defines LIBFPV_KAMIKAZE): libfpv.h's fpv_frame differs between the two.
#if defined(LIBFPV_KAMIKAZE)
#define FPV_KAMIKAZE_FUNCTIONS(X)                                                                \
    X(fpv_kamikaze_poll_detonation) X(fpv_kamikaze_get_state) X(fpv_kamikaze_get_config)        \
    X(fpv_kamikaze_set_ending_support)
#else
#define FPV_KAMIKAZE_FUNCTIONS(X)
#endif

struct Api {
#define FPV_FN(name) decltype(&::name) name = nullptr;
    FPV_FUNCTIONS(FPV_FN)
    FPV_KAMIKAZE_FUNCTIONS(FPV_FN)
#undef FPV_FN
};

extern Api fpv;

// Loads and initialises libfpv once; later calls return the first result.
bool load();
bool loaded();

// The loaded libfpv.dll carries a warhead (a kamikaze build).
bool kamikaze();

// What the player chose for Niko in F8 > Fly > Pilot (fpv_pilot_features and
// the stand-in distances, metres).
fpv_pilot_config pilot_config();

// The game's own controls (fpv_controls_set), read with fpv_control_poll.
enum Control : uint32_t {
    kControlCamera = 1,    // C: FPV, chase, line of sight
    kControlRespawn = 2,   // R
    kControlPilot = 3,     // F: back to Niko, the drone left in the world
    kControlMoveNiko = 4,  // Y: Move Niko here
};
// Presses of a control since the last call (0 before libfpv is loaded).
uint32_t presses(Control c);

}  // namespace fpvapi
