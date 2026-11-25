// GTA IV FPV drone mode, powered by libfpv. This is libfpv/libfpv_gtaiv.dll;
// the libfpv.asi loader calls it every script tick and hot-reloads it.
#include <windows.h>

#include "autotest.h"
#include "drone.h"
#include "log.h"
#include "recon.h"
#include "render.h"
#include "video.h"

namespace {

// The first reading of each key is only a baseline (see drone.cpp).
bool key_pressed(int vk) {
    static bool was_down[256], seen[256];
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = seen[vk] && down && !was_down[vk];
    was_down[vk] = down;
    seen[vk] = true;
    return pressed;
}

void tick() {
    static bool first = true;
    if (first) {
        first = false;
        fpvlog::write("first script tick");
        recon::run_once();
    }
    if (key_pressed(VK_F7)) drone::toggle();
    autotest::tick();
    drone::tick();
}

}  // namespace

extern "C" __declspec(dllexport) void libfpv_gtaiv_tick() { tick(); }

// The loader's D3D hook, on the render thread.
extern "C" __declspec(dllexport) void libfpv_gtaiv_render(IDirect3DDevice9 *dev) { render::frame(dev); }
extern "C" __declspec(dllexport) void libfpv_gtaiv_release_d3d() { render::release(); }

// Called before the loader unloads this core: leave nothing pointing into it.
extern "C" __declspec(dllexport) void libfpv_gtaiv_unload() {
    while (drone::active()) drone::toggle();  // from the pilot view: back to the drone, then down
    autotest::abort();
    video::shutdown();  // the shader compile thread runs our code
    fpvlog::write("core unloaded");
}
