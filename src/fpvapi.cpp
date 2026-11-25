#include "fpvapi.h"

#include <windows.h>

#include <filesystem>
#include <string>

#include "build_info.h"
#include "log.h"
#include "paths.h"
#include "video.h"
#include "world.h"

namespace fpvapi {

Api fpv;

namespace {

// The stand-in's distances (standin in drone.cpp), metres: the game's defaults.
constexpr float kStandInDistance = 20.0f, kStandInReturnDistance = 10.0f;

bool g_loaded = false;
bool g_failed = false;

void on_fpv_log(void *, fpv_log_level level, const char *message) {
    static const char *names[] = {"debug", "info", "warn", "error"};
    fpvlog::write("libfpv %s: %s", names[level & 3], message);
}

// The mod's own files in the old fpv-data folder, which are not settings.
bool mod_own_file(const std::filesystem::path &name) {
    return name == "shots" || name == "autotest.txt" || name == "shot.txt" || name == "recon" ||
           name == "gtaiv-fpv.ini";
}

// libfpv's shared folder (%APPDATA%\libfpv), so the radio mappings, the tune
// and the settings follow the pilot between libfpv games and survive a game
// reinstall. The first time, it takes over what earlier versions kept in
// GTAIV\fpv-data, unless another game already created it (then the pilot's
// setup there wins). Falls back to GTAIV\libfpv\data when there is no home folder.
std::string pick_data_dir() {
    namespace fs = std::filesystem;
    std::string fallback = paths::mod_file("data");
    char shared[1024];
    fpv_result r = fpv.fpv_shared_data_dir(shared, sizeof shared);
    if (r != FPV_OK) {
        fpvlog::write("fpv_shared_data_dir: %s, using %s", fpv.fpv_result_string(r), fallback.c_str());
        return fallback;
    }
    // libfpv's path is UTF-8; the game's narrow strings are the ANSI code page.
    fs::path shared_path = fs::u8path(shared), old_path(paths::game_dir() + "\\fpv-data");
    std::error_code ec;
    if (!fs::exists(shared_path, ec) && fs::is_directory(old_path, ec)) {
        fs::create_directories(shared_path, ec);
        for (const auto &entry : fs::directory_iterator(old_path, ec)) {
            if (mod_own_file(entry.path().filename())) continue;
            std::error_code copy_ec;
            fs::copy(entry.path(), shared_path / entry.path().filename(),
                     fs::copy_options::recursive | fs::copy_options::skip_existing, copy_ec);
            if (copy_ec)
                fpvlog::write("cannot copy %s: %s", entry.path().string().c_str(), copy_ec.message().c_str());
        }
        fpvlog::write("moved settings from %s to %s (the old folder is kept)", old_path.string().c_str(), shared);
    }
    return shared;
}

// Default keys outside the keyboard fallback's (W A S D, arrows, Enter, B, T;
// K and X in kamikaze builds) and libfpv's own (F7, F8). F and Y are the keys
// every libfpv game uses for these two. Gamepad buttons: none by default, the
// free ones are few and the player can bind them in F8 > Controls.
void declare_controls() {
    struct Def {
        Control id;
        const char *name, *label;
        int key;
    };
    static const Def kDefs[] = {
        {kControlCamera, "gtaiv.camera", "Switch camera (FPV, chase, line of sight)", 'C'},
        {kControlRespawn, "gtaiv.respawn", "Respawn the drone", 'R'},
        {kControlPilot, "gtaiv.pilot_view", "Back to Niko (the drone stays)", 'F'},
        {kControlMoveNiko, "gtaiv.move_niko", "Move Niko here", 'Y'},
    };
    fpv_control_desc c[4] = {};
    for (int i = 0; i < 4; ++i) {
        c[i].struct_size = sizeof c[i];
        c[i].id = kDefs[i].id;
        c[i].name = kDefs[i].name;
        c[i].label = kDefs[i].label;
        c[i].kind = FPV_CONTROL_INSTANT;
        c[i].key = kDefs[i].key;
        c[i].gamepad_button = FPV_GAMEPAD_NONE;
    }
    fpv_result r = fpv.fpv_controls_set(c, 4);
    if (r != FPV_OK) fpvlog::write("fpv_controls_set: %s", fpv.fpv_result_string(r));
}

}  // namespace

bool loaded() { return g_loaded; }

fpv_pilot_config pilot_config() {
    fpv_pilot_config pc{};
    pc.struct_size = sizeof pc;
    if (!g_loaded || fpv.fpv_pilot_get_config(&pc) != FPV_OK) {
        pc.features = FPV_PILOT_STAND_IN | FPV_PILOT_POSE;
        pc.stand_in_distance = kStandInDistance;
        pc.stand_in_return_distance = kStandInReturnDistance;
    }
    return pc;
}

uint32_t presses(Control c) {
    if (!g_loaded) return 0;
    uint32_t n = 0;
    fpv.fpv_control_poll(c, &n);
    return n;
}

bool kamikaze() {
#if defined(LIBFPV_KAMIKAZE)
    return g_loaded && fpv.fpv_kamikaze_poll_detonation && fpv.fpv_kamikaze_get_state && fpv.fpv_kamikaze_get_config;
#else
    return false;
#endif
}

bool load() {
    if (g_loaded) return true;
    if (g_failed) return false;
    g_failed = true;  // until proven otherwise; don't retry every key press

    std::string dll = paths::mod_file("libfpv.dll");
    HMODULE lib = LoadLibraryA(dll.c_str());
    if (!lib) {
        fpvlog::write("cannot load %s (error %lu)", dll.c_str(), GetLastError());
        return false;
    }
    bool ok = true;
#define FPV_FN(name)                                                                                     \
    fpv.name = reinterpret_cast<decltype(fpv.name)>(reinterpret_cast<void *>(GetProcAddress(lib, #name))); \
    if (!fpv.name) {                                                                                     \
        fpvlog::write("libfpv.dll has no %s: it is older than this mod, install both from one ZIP", #name); \
        ok = false;                                                                                      \
    }
    FPV_FUNCTIONS(FPV_FN)
#undef FPV_FN
    if (!ok) return false;
#define FPV_FN(name) \
    fpv.name = reinterpret_cast<decltype(fpv.name)>(reinterpret_cast<void *>(GetProcAddress(lib, #name)));
    FPV_KAMIKAZE_FUNCTIONS(FPV_FN)
#undef FPV_FN
    // libfpv.h lays fpv_frame out differently with LIBFPV_KAMIKAZE (12 radio
    // channels, not 10): every field after channels would be read 8 bytes off.
    bool dll_kamikaze = GetProcAddress(lib, "fpv_kamikaze_poll_detonation") != nullptr;
#if defined(LIBFPV_KAMIKAZE)
    const bool mod_kamikaze = true;
#else
    const bool mod_kamikaze = false;
#endif
    if (dll_kamikaze != mod_kamikaze) {
        fpvlog::write("libfpv.dll is a %s build but this mod was built for a %s one: not loaded (build the mod with "
                      "-DGTAIV_FPV_KAMIKAZE=%s)",
                      dll_kamikaze ? "kamikaze" : "plain", mod_kamikaze ? "kamikaze" : "plain",
                      dll_kamikaze ? "ON" : "OFF");
        return false;
    }

    std::string data_dir = pick_data_dir();
    fpv_global_config cfg;
    fpv.fpv_global_config_defaults(&cfg);
    cfg.data_dir = data_dir.c_str();
    // GTA IV: metres, X east, Y north, Z up.
    cfg.coords = {FPV_AXIS_POS_X, FPV_AXIS_POS_Z, FPV_AXIS_POS_Y, 1.0f};
    // Prefer the loader's callback: libfpv keeps it across core reloads.
    if (auto loader_log = reinterpret_cast<fpv_log_fn>(reinterpret_cast<void *>(
            GetProcAddress(GetModuleHandleA("libfpv.asi"), "libfpv_gtaiv_log"))))
        cfg.log = loader_log;
    else
        cfg.log = &on_fpv_log;
    cfg.log_level = FPV_LOG_INFO;
    // video.cpp draws the video feed (noise, the camera's resolution and
    // aspect) when Windows has the shader compiler: then F8 offers it.
    if (video::start()) cfg.flags |= FPV_INIT_VIDEO_FEED;
    // F8 > About: the mod's build next to libfpv's, for bug reports.
    cfg.host_name = "GTA IV FPV";
    cfg.host_build = GTAIV_FPV_BUILD;
    cfg.host_url = "https://github.com/hammerill/gtaiv-fpv";
    fpv_result r = fpv.fpv_global_init(&cfg);
    if (r != FPV_OK && r != FPV_ERROR_ALREADY_INITIALIZED) {
        fpvlog::write("fpv_global_init: %s (%s)", fpv.fpv_result_string(r), fpv.fpv_last_error());
        return false;
    }
    // Liberty City is asphalt, concrete and brick: rough ground (0.6), and a
    // car's paint under the frame (0.5), so a perched drone doesn't slide
    // about while the car speeds up (world.cpp gives hits these materials).
    fpv_material ground{0.2f, 0.6f, 0}, paint{0.2f, 0.5f, 0};
    fpv.fpv_material_set(world::kMaterialGround, &ground);
    fpv.fpv_material_set(world::kMaterialVehicle, &paint);
    // Niko stands in for a far drone and poses with the radio (drone.cpp, pilot.cpp).
    fpv.fpv_pilot_set_support(FPV_PILOT_STAND_IN | FPV_PILOT_POSE, kStandInDistance, kStandInReturnDistance);
    declare_controls();
    g_failed = false;
    g_loaded = true;
    fpvlog::write("libfpv %s initialised (%s, video feed %s), build %s, data in %s", fpv.fpv_version_string(),
                  kamikaze() ? "kamikaze build" : "no warhead", (cfg.flags & FPV_INIT_VIDEO_FEED) ? "on" : "off",
                  GTAIV_FPV_BUILD, data_dir.c_str());
    return true;
}

}  // namespace fpvapi
