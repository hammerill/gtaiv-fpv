// The video feed (libfpv's frame.video_feed, docs/INTEGRATION.md#the-video-feed)
// drawn with Direct3D 9 by libfpv's docs/renderers/fpv_video_d3d9.h: the
// analog link's noise, digital breakup, and the camera's resolution and aspect
// with black bars, plus the lens.
#pragma once

#include <d3d9.h>

#include "libfpv.h"

namespace video {

// Starts compiling the shaders on a thread (d3dcompiler_47.dll). False when the
// compiler is missing: then the game must not opt in to the video feed.
bool start();

// Draws an OSD's draw data into the bound render target of width x height,
// scaled from its display_size (fixed-function state included).
using DrawUi = void (*)(IDirect3DDevice9 *dev, const fpv_draw_data &dd, float width, float height);

// The goggles, over the whole bound render target (the back buffer, fb_w x
// fb_h): `scene` is the finished game frame, rendered from frame.camera. `ui`
// (or null) is the game UI kept in flight, fb_w x fb_h, premultiplied: it goes
// into the camera's picture after the lens and under the OSD, so the feed's
// noise covers it but the lens doesn't bend it. Draws the OSD too when
// draw_osd. False when it can't (shaders not ready, no ps_3_0): then draw the
// view the old way.
bool draw(IDirect3DDevice9 *dev, IDirect3DTexture9 *scene, IDirect3DTexture9 *ui, UINT scene_w, UINT scene_h,
          UINT fb_w, UINT fb_h, const fpv_frame &f, bool draw_osd, DrawUi draw_ui);

// The feed's flags in a frame (0 for an older libfpv that fills none).
uint32_t flags(const fpv_frame &f);

// Whether draw() can work on this device (after the shaders are ready).
bool usable();

// Releases the device's resources (device lost, core unload).
void release();

// Waits for the compile thread (core unload).
void shutdown();

}  // namespace video
