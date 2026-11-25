#include "render.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

#include "drone.h"
#include "fpvapi.h"
#include "log.h"
#include "paths.h"
#include "video.h"

namespace render {
namespace {

using fpvapi::fpv;

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct Texture {
    IDirect3DTexture9 *tex = nullptr;
    uint32_t revision = 0;
};
Texture g_textures[3];  // indexed by FPV_TEXTURE_* (1 overlay font, 2 OSD font)
std::vector<Vertex> g_vertices;
LARGE_INTEGER g_last;
std::atomic<float> g_aspect{16.0f / 9.0f};

// Fisheye: a copy of the finished frame, drawn back through a warped grid.
IDirect3DTexture9 *g_scene = nullptr;
UINT g_scene_w = 0, g_scene_h = 0;
D3DFORMAT g_scene_format = D3DFMT_UNKNOWN;
constexpr int kGridX = 64, kGridY = 36;  // cells; the warp is smooth, so this is exact to a pixel
std::vector<Vertex> g_grid;
std::vector<uint16_t> g_grid_indices;

// Signal lost: analog static, regenerated every frame and stretched to the screen.
constexpr UINT kStaticW = 320, kStaticH = 180;
IDirect3DTexture9 *g_static = nullptr;
uint32_t g_noise_seed = 0x2545f491u;

IDirect3DTexture9 *texture(IDirect3DDevice9 *dev, uint64_t id) {
    if (id >= 3) return nullptr;
    fpv_texture t{};
    if (fpv.fpv_texture_get(id, &t) != FPV_OK || !t.rgba) return nullptr;
    Texture &slot = g_textures[id];
    if (slot.tex && slot.revision == t.revision) return slot.tex;
    if (slot.tex) slot.tex->Release();
    slot.tex = nullptr;
    if (FAILED(dev->CreateTexture(t.width, t.height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &slot.tex, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(slot.tex->LockRect(0, &lr, nullptr, 0))) {
        for (uint32_t y = 0; y < t.height; ++y) {
            const uint8_t *src = t.rgba + size_t(y) * t.width * 4;
            uint8_t *dst = static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch;
            for (uint32_t x = 0; x < t.width; ++x, src += 4, dst += 4) {  // RGBA -> BGRA
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[3];
            }
        }
        slot.tex->UnlockRect(0);
    }
    slot.revision = t.revision;
    return slot.tex;
}

// Fixed-function state for alpha-blended screen-space quads, as in Dear
// ImGui's DX9 backend.
void setup_state(IDirect3DDevice9 *dev, float w, float h) {
    D3DVIEWPORT9 vp = {0, 0, DWORD(w), DWORD(h), 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(kFvf);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
}

void draw(IDirect3DDevice9 *dev, const fpv_draw_data &dd, float sx = 1, float sy = 1) {
    for (uint32_t l = 0; l < dd.list_count; ++l) {
        const fpv_draw_list &list = dd.lists[l];
        g_vertices.resize(list.vertex_count);
        for (uint32_t i = 0; i < list.vertex_count; ++i) {
            const fpv_draw_vertex &s = list.vertices[i];
            uint32_t c = s.color;  // ImGui ABGR -> D3D ARGB
            c = (c & 0xff00ff00u) | ((c & 0xffu) << 16) | ((c >> 16) & 0xffu);
            g_vertices[i] = {s.pos[0] * sx - 0.5f, s.pos[1] * sy - 0.5f, 0.0f, 1.0f, c, s.uv[0], s.uv[1]};
        }
        for (uint32_t c = 0; c < list.command_count; ++c) {
            const fpv_draw_cmd &cmd = list.commands[c];
            if (!cmd.index_count || cmd.vertex_offset >= list.vertex_count) continue;
            RECT clip = {LONG(cmd.clip_rect[0] * sx), LONG(cmd.clip_rect[1] * sy), LONG(cmd.clip_rect[2] * sx),
                         LONG(cmd.clip_rect[3] * sy)};
            if (clip.right <= clip.left || clip.bottom <= clip.top) continue;
            dev->SetScissorRect(&clip);
            dev->SetTexture(0, texture(dev, cmd.texture_id));
            dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, list.vertex_count - cmd.vertex_offset,
                                        cmd.index_count / 3, list.indices + cmd.index_offset, D3DFMT_INDEX32,
                                        g_vertices.data() + cmd.vertex_offset, sizeof(Vertex));
        }
    }
}

// libfpv's lens (docs/INTEGRATION.md#lens-distortion) as a mesh instead of a
// pixel shader: each grid vertex at output position uv samples the copied
// scene at c * (1 + k1 r2 + k2 r2^2) / zoom, with |c| = 1 at the corners.
// The game drew the frame with the widened FOV from frame.camera.
// The finished game frame, copied into g_scene (the lens and the video feed sample it).
bool copy_scene(IDirect3DDevice9 *dev, IDirect3DSurface9 *backbuffer, const D3DSURFACE_DESC &desc) {
    if (!g_scene || g_scene_w != desc.Width || g_scene_h != desc.Height || g_scene_format != desc.Format) {
        if (g_scene) g_scene->Release();
        g_scene = nullptr;
        if (FAILED(dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
                                      &g_scene, nullptr)))
            return false;
        g_scene_w = desc.Width, g_scene_h = desc.Height, g_scene_format = desc.Format;
    }
    IDirect3DSurface9 *copy = nullptr;
    if (FAILED(g_scene->GetSurfaceLevel(0, &copy))) return false;
    HRESULT hr = dev->StretchRect(backbuffer, nullptr, copy, nullptr, D3DTEXF_NONE);
    copy->Release();
    return SUCCEEDED(hr);
}

bool draw_lens(IDirect3DDevice9 *dev, IDirect3DSurface9 *backbuffer, const D3DSURFACE_DESC &desc,
               const fpv_camera &cam) {
    if (!copy_scene(dev, backbuffer, desc)) return false;

    const float w = float(desc.Width), h = float(desc.Height), a = w / h;
    const float sx = a / (0.5f * std::sqrt(a * a + 1)), sy = 1 / (0.5f * std::sqrt(a * a + 1));
    const float k1 = cam.distortion_k1, k2 = cam.distortion_k2, zoom = cam.distortion_zoom > 0 ? cam.distortion_zoom : 1;
    g_grid.resize((kGridX + 1) * (kGridY + 1));
    for (int y = 0; y <= kGridY; ++y)
        for (int x = 0; x <= kGridX; ++x) {
            float u = float(x) / kGridX, v = float(y) / kGridY;
            float cx = (u - 0.5f) * sx, cy = (v - 0.5f) * sy, r2 = cx * cx + cy * cy;
            float m = (1 + k1 * r2 + k2 * r2 * r2) / zoom;
            g_grid[y * (kGridX + 1) + x] = {u * w - 0.5f, v * h - 0.5f, 0.0f, 1.0f, 0xffffffffu,
                                            cx * m / sx + 0.5f, cy * m / sy + 0.5f};
        }
    if (g_grid_indices.empty())
        for (int y = 0; y < kGridY; ++y)
            for (int x = 0; x < kGridX; ++x) {
                uint16_t i = uint16_t(y * (kGridX + 1) + x), j = uint16_t(i + kGridX + 1);
                for (uint16_t k : {i, uint16_t(i + 1), j, uint16_t(i + 1), uint16_t(j + 1), j}) g_grid_indices.push_back(k);
            }
    RECT full = {0, 0, LONG(desc.Width), LONG(desc.Height)};
    dev->SetScissorRect(&full);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetTexture(0, g_scene);
    dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, UINT(g_grid.size()), UINT(g_grid_indices.size() / 3),
                                g_grid_indices.data(), D3DFMT_INDEX16, g_grid.data(), sizeof(Vertex));
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    return true;
}

uint32_t noise() {
    g_noise_seed ^= g_noise_seed << 13, g_noise_seed ^= g_noise_seed >> 17, g_noise_seed ^= g_noise_seed << 5;
    return g_noise_seed;
}

// Grey snow with brighter and darker rolling bands, like an analog receiver
// that lost its transmitter.
bool draw_static(IDirect3DDevice9 *dev, float w, float h) {
    if (!g_static &&
        FAILED(dev->CreateTexture(kStaticW, kStaticH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_static, nullptr)))
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED(g_static->LockRect(0, &lr, nullptr, 0))) return false;
    static float band_phase = 0;
    band_phase += 0.07f + float(noise() & 0xff) / 255.0f * 0.05f;
    for (UINT y = 0; y < kStaticH; ++y) {
        float band = 0.75f + 0.25f * std::sin(float(y) * 0.045f + band_phase);
        if ((noise() & 63) == 0) band *= 1.35f;  // a torn line now and then
        uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch);
        for (UINT x = 0; x < kStaticW; ++x) {
            float v = float(noise() & 0xff) * band;
            uint32_t g = v > 255 ? 255 : uint32_t(v);
            row[x] = 0xff000000u | g << 16 | g << 8 | g;
        }
    }
    g_static->UnlockRect(0);
    Vertex quad[4] = {{-0.5f, -0.5f, 0, 1, 0xffffffffu, 0, 0},
                      {w - 0.5f, -0.5f, 0, 1, 0xffffffffu, 1, 0},
                      {-0.5f, h - 0.5f, 0, 1, 0xffffffffu, 0, 1},
                      {w - 0.5f, h - 0.5f, 0, 1, 0xffffffffu, 1, 1}};
    RECT full = {0, 0, LONG(w), LONG(h)};
    dev->SetScissorRect(&full);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetTexture(0, g_static);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    return true;
}

// Self-test screenshots: creating libfpv/shot.txt saves the next finished
// frame (with our passes) to libfpv/shots/<time>.bmp and deletes the file.
// Window capture from outside does not always work (a WSL in session 0).
void save_screenshot(IDirect3DDevice9 *dev, IDirect3DSurface9 *backbuffer, const D3DSURFACE_DESC &desc) {
    static int frames = 0;
    static const std::string trigger = paths::mod_file("shot.txt"), dir = paths::mod_file("shots");
    if (++frames % 10 != 0 || GetFileAttributesA(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(trigger.c_str());
    IDirect3DSurface9 *rt = nullptr, *mem = nullptr;
    bool ok = SUCCEEDED(dev->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                &rt, nullptr)) &&
              SUCCEEDED(dev->StretchRect(backbuffer, nullptr, rt, nullptr, D3DTEXF_NONE)) &&
              SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                        &mem, nullptr)) &&
              SUCCEEDED(dev->GetRenderTargetData(rt, mem));
    D3DLOCKED_RECT lr;
    if (ok && SUCCEEDED(mem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        CreateDirectoryA(dir.c_str(), nullptr);
        SYSTEMTIME t;
        GetLocalTime(&t);
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%02d%02d%02d-%03d.bmp", dir.c_str(), t.wHour, t.wMinute, t.wSecond,
                 t.wMilliseconds);
        if (FILE *f = std::fopen(path, "wb")) {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof ih;
            ih.biWidth = LONG(desc.Width);
            ih.biHeight = -LONG(desc.Height);  // top-down
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fh.bfType = 0x4d42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + desc.Width * desc.Height * 4;
            std::fwrite(&fh, sizeof fh, 1, f);
            std::fwrite(&ih, sizeof ih, 1, f);
            for (UINT y = 0; y < desc.Height; ++y)
                std::fwrite(static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch, 4, desc.Width, f);
            std::fclose(f);
            fpvlog::write("render: screenshot %s", path);
        }
        mem->UnlockRect();
    } else {
        fpvlog::write("render: screenshot failed (format %d)", int(desc.Format));
    }
    if (mem) mem->Release();
    if (rt) rt->Release();
}

// Mouse for the overlay, in back buffer pixels.
void feed_mouse(IDirect3DDevice9 *dev, float w, float h) {
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow) return;
    POINT p;
    RECT rc;
    if (!GetCursorPos(&p) || !ScreenToClient(cp.hFocusWindow, &p) || !GetClientRect(cp.hFocusWindow, &rc)) return;
    float sx = rc.right > 0 ? w / float(rc.right) : 1.0f, sy = rc.bottom > 0 ? h / float(rc.bottom) : 1.0f;
    fpv.fpv_overlay_mouse_move(float(p.x) * sx, float(p.y) * sy);
    static bool was[3];
    const int vk[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    for (int b = 0; b < 3; ++b) {
        bool down = (GetAsyncKeyState(vk[b]) & 0x8000) != 0;
        if (down != was[b]) fpv.fpv_overlay_mouse_button(b, down);
        was[b] = down;
    }
}


// ---------------------------------------------------------------------------
// The drone model, in the chase, LOS and pilot views (libfpv INTEGRATION.md
// "The drone model"). GTA IV can't load a mod's model from a script hook, and
// this pass runs after the frame, without the game's transforms or a depth
// buffer we can trust. So it projects a low-poly 5" quad itself through the
// camera the frame was rendered from (drone::ModelPose), sorts its triangles
// back to front, and is skipped when the drone is behind something (a ray
// from the camera, drone.cpp). No shadow: that would need the game's renderer.

struct Tri {
    float depth;
    Vertex v[3];
};
std::vector<Tri> g_tris;

struct Projector {
    const drone::ModelPose &m;
    float w, h, ty, tx;  // screen size, tan of the half FOVs
    // body-local (right, forward, up), metres -> world
    void world(const float l[3], float out[3]) const {
        for (int i = 0; i < 3; ++i) out[i] = m.pos[i] + m.right[i] * l[0] + m.forward[i] * l[1] + m.up[i] * l[2];
    }
    // world -> screen; false behind the near plane
    bool screen(const float p[3], Vertex &v, float &depth) const {
        float d[3] = {p[0] - m.eye[0], p[1] - m.eye[1], p[2] - m.eye[2]};
        float z = d[0] * m.eye_forward[0] + d[1] * m.eye_forward[1] + d[2] * m.eye_forward[2];
        if (z < 0.05f) return false;
        float x = d[0] * m.eye_right[0] + d[1] * m.eye_right[1] + d[2] * m.eye_right[2];
        float y = d[0] * m.eye_up[0] + d[1] * m.eye_up[1] + d[2] * m.eye_up[2];
        v.x = (0.5f + 0.5f * x / (z * tx)) * w - 0.5f;
        v.y = (0.5f - 0.5f * y / (z * ty)) * h - 0.5f;
        v.z = 0.5f;
        v.rhw = 1.0f / z;
        v.u = v.v = 0;
        depth = z;
        return true;
    }
};

DWORD shade(DWORD argb, const float n_world[3], float light) {
    // a sun from above and a little south-east, plus ambient
    static const float kSun[3] = {0.35f, -0.35f, 0.87f};
    float d = n_world[0] * kSun[0] + n_world[1] * kSun[1] + n_world[2] * kSun[2];
    float k = (0.45f + 0.55f * (d > 0 ? d : 0)) * light;
    auto ch = [&](int shift) { return DWORD(float((argb >> shift) & 0xff) * k) & 0xff; };
    return (argb & 0xff000000u) | ch(16) << 16 | ch(8) << 8 | ch(0);
}

void add_tri(const Projector &pr, const float a[3], const float b[3], const float c[3], DWORD color, float light,
             bool lit = true) {
    float wa[3], wb[3], wc[3];
    pr.world(a, wa), pr.world(b, wb), pr.world(c, wc);
    float e1[3] = {wb[0] - wa[0], wb[1] - wa[1], wb[2] - wa[2]}, e2[3] = {wc[0] - wa[0], wc[1] - wa[1], wc[2] - wa[2]};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl < 1e-9f) return;
    for (float &v : n) v /= nl;
    // two-sided: face the normal to the camera
    float to_eye = (pr.m.eye[0] - wa[0]) * n[0] + (pr.m.eye[1] - wa[1]) * n[1] + (pr.m.eye[2] - wa[2]) * n[2];
    if (to_eye < 0)
        for (float &v : n) v = -v;
    Tri t;
    float d0, d1, d2;
    if (!pr.screen(wa, t.v[0], d0) || !pr.screen(wb, t.v[1], d1) || !pr.screen(wc, t.v[2], d2)) return;
    DWORD c2 = lit ? shade(color, n, light) : color;
    for (Vertex &v : t.v) v.color = c2;
    t.depth = (d0 + d1 + d2) / 3;
    g_tris.push_back(t);
}

// An axis-aligned box in body coordinates.
void add_box(const Projector &pr, const float c[3], const float half[3], DWORD color, float light) {
    float p[8][3];
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 3; ++k) p[i][k] = c[k] + ((i >> k) & 1 ? half[k] : -half[k]);
    static const int kFaces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
    for (const auto &f : kFaces) {
        add_tri(pr, p[f[0]], p[f[1]], p[f[2]], color, light);
        add_tri(pr, p[f[0]], p[f[2]], p[f[3]], color, light);
    }
}

// A thin bar from a to b (body coordinates), width w, height t.
void add_bar(const Projector &pr, const float a[3], const float b[3], float w, float t, DWORD color, float light) {
    float d[3] = {b[0] - a[0], b[1] - a[1], 0};
    float l = std::sqrt(d[0] * d[0] + d[1] * d[1]);
    if (l < 1e-4f) return;
    float s[3] = {-d[1] / l * w * 0.5f, d[0] / l * w * 0.5f, 0};
    float p[8][3];
    for (int i = 0; i < 8; ++i) {
        const float *e = (i & 1) ? b : a;
        float side = (i & 2) ? 1.0f : -1.0f, up = (i & 4) ? t * 0.5f : -t * 0.5f;
        for (int k = 0; k < 3; ++k) p[i][k] = e[k] + s[k] * side + (k == 2 ? up : 0);
    }
    static const int kFaces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (const auto &f : kFaces) {
        add_tri(pr, p[f[0]], p[f[1]], p[f[2]], color, light);
        add_tri(pr, p[f[0]], p[f[2]], p[f[3]], color, light);
    }
}

// A horizontal disc (the spinning prop's blur) or two blades.
void add_prop(const Projector &pr, const float hub[3], float radius, float rpm, float spin, float light) {
    if (rpm > 1500) {
        const int kSeg = 16;
        for (int i = 0; i < kSeg; ++i) {
            float a0 = 6.2831853f * i / kSeg, a1 = 6.2831853f * (i + 1) / kSeg;
            float p0[3] = {hub[0] + radius * std::cos(a0), hub[1] + radius * std::sin(a0), hub[2]};
            float p1[3] = {hub[0] + radius * std::cos(a1), hub[1] + radius * std::sin(a1), hub[2]};
            add_tri(pr, hub, p0, p1, 0xa0505058u, light, false);  // a grey blur, visible on dark roofs too
        }
        return;
    }
    for (int blade = 0; blade < 2; ++blade) {
        float a = spin + blade * 3.14159265f;
        float tip[3] = {hub[0] + radius * std::cos(a), hub[1] + radius * std::sin(a), hub[2]};
        add_bar(pr, hub, tip, 0.012f, 0.002f, 0xff1a1a1au, light);
    }
}

void draw_drone_model(IDirect3DDevice9 *dev, float w, float h) {
    drone::ModelPose m;
    if (!drone::model_pose(&m)) return;
    Projector pr{m, w, h, std::tan(m.fov_y * 0.5f / 57.2957795f), 0};
    pr.tx = pr.ty * w / h;
    g_tris.clear();
    const float light = m.light;
    // body, battery on top, camera at the front
    const float body[3] = {0, 0, 0}, body_half[3] = {0.035f, 0.07f, 0.018f};
    const float pack[3] = {0, -0.005f, 0.035f}, pack_half[3] = {0.02f, 0.045f, 0.017f};
    const float cam[3] = {0, 0.075f, 0.005f}, cam_half[3] = {0.012f, 0.008f, 0.012f};
    add_box(pr, body, body_half, 0xff2a2a2eu, light);
    add_box(pr, pack, pack_half, 0xffc8a020u, light);
    add_box(pr, cam, cam_half, 0xff101010u, light);
    for (int i = 0; i < 4; ++i) {
        const float *hub = m.motor[i];
        add_bar(pr, body, hub, 0.014f, 0.005f, 0xff222226u, light);
        const float bell[3] = {hub[0], hub[1], hub[2] + 0.008f}, bell_half[3] = {0.011f, 0.011f, 0.01f};
        add_box(pr, bell, bell_half, 0xff8a8a90u, light);
        if (m.prop_gone[i]) continue;
        const float prop[3] = {hub[0], hub[1], hub[2] + 0.02f};
        add_prop(pr, prop, m.prop_radius, m.rpm[i], m.spin[i], light);
    }
    if (g_tris.empty()) return;
    std::sort(g_tris.begin(), g_tris.end(), [](const Tri &a, const Tri &b) { return a.depth > b.depth; });
    g_vertices.resize(g_tris.size() * 3);
    for (size_t i = 0; i < g_tris.size(); ++i)
        for (int k = 0; k < 3; ++k) g_vertices[i * 3 + k] = g_tris[i].v[k];
    RECT full = {0, 0, LONG(w), LONG(h)};
    dev->SetScissorRect(&full);
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(g_tris.size()), g_vertices.data(), sizeof(Vertex));
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
}

// Draws libfpv draw data (the OSD) for the video feed, which binds its own state.
void draw_ui(IDirect3DDevice9 *dev, const fpv_draw_data &dd, float w, float h) {
    setup_state(dev, w, h);
    float dw = dd.display_size[0] > 0 ? dd.display_size[0] : w, dh = dd.display_size[1] > 0 ? dd.display_size[1] : h;
    draw(dev, dd, w / dw, h / dh);
}

// Everything setup_state() and draw() change, so it can be put back.
const D3DRENDERSTATETYPE kRenderStates[] = {
    D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE,
    D3DRS_ALPHABLENDENABLE, D3DRS_BLENDOP, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_RANGEFOGENABLE,
    D3DRS_SPECULARENABLE, D3DRS_STENCILENABLE, D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_SRGBWRITEENABLE,
    D3DRS_COLORWRITEENABLE, D3DRS_BLENDOPALPHA,
};
const D3DTEXTURESTAGESTATETYPE kStage0States[] = {
    D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
    D3DTSS_TEXTURETRANSFORMFLAGS,
};
const D3DSAMPLERSTATETYPE kSamplerStates[] = {
    D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
};
constexpr size_t kNumRs = sizeof kRenderStates / sizeof kRenderStates[0];
constexpr size_t kNumTss = sizeof kStage0States / sizeof kStage0States[0];
constexpr size_t kNumSs = sizeof kSamplerStates / sizeof kSamplerStates[0];

struct SavedState {
    DWORD rs[kNumRs], tss0[kNumTss], tss1_color, tss1_alpha, ss[kNumSs];
    D3DVIEWPORT9 viewport;
    RECT scissor;
    IDirect3DBaseTexture9 *texture = nullptr;
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    DWORD fvf;
    IDirect3DVertexBuffer9 *stream = nullptr;
    UINT stream_offset, stream_stride;
    IDirect3DIndexBuffer9 *indices = nullptr;

    void save(IDirect3DDevice9 *dev) {
        for (size_t i = 0; i < kNumRs; ++i) dev->GetRenderState(kRenderStates[i], &rs[i]);
        for (size_t i = 0; i < kNumTss; ++i) dev->GetTextureStageState(0, kStage0States[i], &tss0[i]);
        dev->GetTextureStageState(1, D3DTSS_COLOROP, &tss1_color);
        dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &tss1_alpha);
        for (size_t i = 0; i < kNumSs; ++i) dev->GetSamplerState(0, kSamplerStates[i], &ss[i]);
        dev->GetViewport(&viewport);
        dev->GetScissorRect(&scissor);
        dev->GetTexture(0, &texture);
        dev->GetVertexShader(&vs);
        dev->GetPixelShader(&ps);
        dev->GetVertexDeclaration(&decl);
        dev->GetFVF(&fvf);
        dev->GetStreamSource(0, &stream, &stream_offset, &stream_stride);
        dev->GetIndices(&indices);
    }

    // Releases the references save() took.
    void restore(IDirect3DDevice9 *dev) {
        for (size_t i = 0; i < kNumRs; ++i) dev->SetRenderState(kRenderStates[i], rs[i]);
        for (size_t i = 0; i < kNumTss; ++i) dev->SetTextureStageState(0, kStage0States[i], tss0[i]);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, tss1_color);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, tss1_alpha);
        for (size_t i = 0; i < kNumSs; ++i) dev->SetSamplerState(0, kSamplerStates[i], ss[i]);
        dev->SetViewport(&viewport);
        dev->SetScissorRect(&scissor);
        dev->SetTexture(0, texture);
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        if (decl)
            dev->SetVertexDeclaration(decl);
        else
            dev->SetFVF(fvf);
        dev->SetStreamSource(0, stream, stream_offset, stream_stride);
        dev->SetIndices(indices);
        for (IUnknown *u : {static_cast<IUnknown *>(texture), static_cast<IUnknown *>(vs), static_cast<IUnknown *>(ps),
                            static_cast<IUnknown *>(decl), static_cast<IUnknown *>(stream),
                            static_cast<IUnknown *>(indices)})
            if (u) u->Release();
    }
};

}  // namespace

void frame(IDirect3DDevice9 *dev) {
    if (!fpvapi::loaded()) {
        IDirect3DSurface9 *backbuffer = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) return;
        D3DSURFACE_DESC desc;
        backbuffer->GetDesc(&desc);
        g_aspect = float(desc.Width) / float(desc.Height);
        save_screenshot(dev, backbuffer, desc);
        backbuffer->Release();
        return;
    }

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_last.QuadPart ? float(now.QuadPart - g_last.QuadPart) / float(freq.QuadPart) : 0.016f;
    g_last = now;

    IDirect3DSurface9 *backbuffer = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) return;
    D3DSURFACE_DESC desc;
    backbuffer->GetDesc(&desc);
    float w = float(desc.Width), h = float(desc.Height);

    g_aspect = w / h;

    fpv.fpv_overlay_set_display(w, h, h > 1080.0f ? h / 1080.0f : 1.0f);
    if (fpv.fpv_overlay_is_open()) feed_mouse(dev, w, h);

    static fpv_frame snapshot;
    const fpv_draw_data *osd = nullptr;
    bool no_signal = false;
    bool fpv_view = drone::osd_frame(&snapshot, &no_signal);
    bool lens = fpv_view && !no_signal && (snapshot.camera.distortion_k1 != 0 || snapshot.camera.distortion_k2 != 0);
    bool want_osd = fpv_view && (drone::tuning().osd || no_signal);
    // The video feed (the player turns it on in F8 > Camera): the goggles
    // draw the lens and the OSD too. A lost feed is its own snow while the
    // noise is on; with it off, our static stands in.
    uint32_t feed = fpv_view ? video::flags(snapshot) : 0;
    bool goggles = feed && (!no_signal || (feed & FPV_VIDEO_FEED_NOISE)) && video::usable();
    if (want_osd && !goggles) osd = fpv.fpv_osd_draw_data(&snapshot, w, h, FPV_OSD_STYLE_ANALOG);
    // Every frame, also with the overlay closed: libfpv's collision view draws through it.
    const fpv_draw_data *overlay = fpv.fpv_overlay_render(dt);
    bool model = !fpv_view && drone::active();
    if (!goggles && !lens && !no_signal && !model && (!osd || !osd->list_count) && (!overlay || !overlay->list_count)) {
        save_screenshot(dev, backbuffer, desc);
        backbuffer->Release();
        return;
    }

    // Draw straight onto the bound back buffer and restore exactly the state
    // we touch. A D3DSBT_ALL state block plus a render target / depth swap
    // here made the game's next frame flicker (stale regions, exposure jumps).
    // (fpv_video_d3d9.h takes a state block of its own: only while the feed is on.)
    IDirect3DSurface9 *rt = nullptr;
    dev->GetRenderTarget(0, &rt);
    bool on_backbuffer = rt == backbuffer;
    if (rt) rt->Release();
    if (!on_backbuffer) {
        backbuffer->Release();
        return;
    }

    SavedState saved;
    saved.save(dev);
    setup_state(dev, w, h);
    if (model) draw_drone_model(dev, w, h);
    if (goggles && !(copy_scene(dev, backbuffer, desc) &&
                     video::draw(dev, g_scene, nullptr, desc.Width, desc.Height, desc.Width, desc.Height, snapshot,
                                 want_osd, &draw_ui))) {
        static int reported = 0;
        if (reported++ < 3) fpvlog::write("render: video feed pass failed: the plain view instead");
        goggles = false;
        setup_state(dev, w, h);
        if (want_osd) osd = fpv.fpv_osd_draw_data(&snapshot, w, h, FPV_OSD_STYLE_ANALOG);
    }
    if (goggles) setup_state(dev, w, h);  // for the overlay
    if (!goggles && lens && !draw_lens(dev, backbuffer, desc, snapshot.camera)) {
        static int reported = 0;
        if (reported++ < 3) fpvlog::write("render: fisheye pass failed (format %d)", int(desc.Format));
    }
    if (!goggles && no_signal && !draw_static(dev, w, h)) {
        static int reported = 0;
        if (reported++ < 3) fpvlog::write("render: static pass failed");
    }
    if (osd) draw(dev, *osd);
    if (overlay) draw(dev, *overlay);
    saved.restore(dev);
    save_screenshot(dev, backbuffer, desc);
    backbuffer->Release();
}

float aspect() { return g_aspect; }

void release() {
    video::release();
    if (g_scene) g_scene->Release();
    g_scene = nullptr;
    g_scene_w = g_scene_h = 0;
    if (g_static) g_static->Release();
    g_static = nullptr;
    for (Texture &t : g_textures) {
        if (t.tex) t.tex->Release();
        t = Texture{};
    }
}

}  // namespace render
