// The video feed drawn with libfpv's docs/renderers/fpv_video_d3d9.h: the
// shaders and the drawing live in libfpv, so a renderer update there only
// needs a rebuild here. This file runs the shader compile on a thread and
// routes libfpv's calls through the runtime-loaded libfpv.dll.
#include "video.h"

#include <windows.h>

#include <atomic>

#include "fpvapi.h"
#include "log.h"

namespace {

fpv_result texture_get(uint64_t id, fpv_texture *out) {
    return fpvapi::fpv.fpv_texture_get ? fpvapi::fpv.fpv_texture_get(id, out) : FPV_ERROR_NOT_FOUND;
}

const fpv_draw_data *osd_draw_data(const fpv_frame *f, float w, float h, fpv_osd_style style) {
    return fpvapi::fpv.fpv_osd_draw_data ? fpvapi::fpv.fpv_osd_draw_data(f, w, h, style) : nullptr;
}

}  // namespace

#define FPV_VIDEO_D3D9_LOG(...) fpvlog::write(__VA_ARGS__)
#define FPV_VIDEO_D3D9_TEXTURE_GET texture_get
#define FPV_VIDEO_D3D9_OSD_DRAW_DATA osd_draw_data
#define FPV_VIDEO_D3D9_IMPLEMENTATION
#include "fpv_video_d3d9.h"

namespace video {
namespace {

fpv_video_d3d9 g_video = {};

// the shaders, compiled on a thread (about a second)
HANDLE g_thread = nullptr;
enum { kIdle, kCompiling, kReady, kFailed };
std::atomic<int> g_state{kIdle};

DWORD WINAPI compile_thread(void *) {
    DWORD start = GetTickCount();
    bool ok = fpv_video_d3d9_compile(&g_video) != 0;
    fpvlog::write("video: shaders %s in %lu ms", ok ? "compiled" : "failed", GetTickCount() - start);
    g_state = ok ? kReady : kFailed;
    return 0;
}

void draw_osd(void *user, IDirect3DDevice9 *dev, const fpv_draw_data *dd, float w, float h) {
    reinterpret_cast<DrawUi>(user)(dev, *dd, w, h);
}

}  // namespace

bool start() {
    if (g_state != kIdle) return g_state != kFailed;
    // without the compiler there is no video feed: the game must not opt in
    if (!LoadLibraryA("d3dcompiler_47.dll")) {
        fpvlog::write("video: no d3dcompiler_47.dll: no video feed");
        g_state = kFailed;
        return false;
    }
    g_state = kCompiling;
    g_thread = CreateThread(nullptr, 0, &compile_thread, nullptr, 0, nullptr);
    if (!g_thread) {
        g_state = kFailed;
        return false;
    }
    return true;
}

uint32_t flags(const fpv_frame &f) { return fpv_video_d3d9_flags(&f); }

bool usable() { return g_state == kReady && fpv_video_d3d9_usable(&g_video); }

bool draw(IDirect3DDevice9 *dev, IDirect3DTexture9 *scene, IDirect3DTexture9 *ui, UINT scene_w, UINT scene_h,
          UINT fb_w, UINT fb_h, const fpv_frame &f, bool osd, DrawUi draw_ui) {
    if (g_state != kReady) return false;
    return fpv_video_d3d9_draw(&g_video, dev, &f, scene, scene_w, scene_h, ui, fb_w, fb_h,
                               osd && draw_ui ? &draw_osd : nullptr, reinterpret_cast<void *>(draw_ui)) != 0;
}

void release() { fpv_video_d3d9_release(&g_video); }

void shutdown() {
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    fpv_video_d3d9_shutdown(&g_video);
    g_state = kIdle;
}

}  // namespace video
