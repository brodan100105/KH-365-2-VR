// The 3D engine: the geometry engine the guest feeds through the GX FIFO and command ports
// (matrices, lighting, texture coordinates, primitives, clipping, the box/position/vector tests)
// and a software rasterizer that draws the polygons of the last SWAP_BUFFERS the way the DS's
// rendering engine does (textures in all seven formats, modulate/decal/toon/highlight, alpha
// test and blending, depth or W buffer, fog). Its picture is engine A's BG0 when DISPCNT puts
// the 3D layer there (render2d.cpp).
//
// The VR view will later take the same polygons as world-space draw lists (docs/ROADMAP.md).
#pragma once

#include <atomic>
#include <cstdint>

#include "khvr/platform.h"

namespace khvr::hle {

using khvr::PlatformWorld;

void GxReset();
void GxWrite(uint32_t addr, uint32_t value, uint32_t size);  // 0x04000400-0x040005ff
uint16_t GxReadRegister(uint32_t off);  // 0x600-0x6ff, offset from 0x04000000
uint32_t GxCommandsThisFrame();
void GxVBlank();

// The 3D layer at `scale` times the DS's 256 x 192 (1 to kMax3dScale): BGR555 colour and 5-bit
// alpha per pixel (alpha 0: nothing there).
constexpr int kMax3dScale = 4;
// `hudOnly`: just the polygons drawn with another projection than the scene's (the flat HUD
// pieces the world capture leaves out).
void GxRender(uint16_t *color, uint8_t *alpha, int scale = 1, bool hudOnly = false);
// The polygons of the last SWAP_BUFFERS drawn with the game's main camera, in world space.
void GxCaptureWorld(PlatformWorld *out);
// What the last capture found, for the heartbeat.
struct GxWorldStats {
    uint32_t captures = 0, triangles = 0, textures = 0, leftOut = 0;
    uint32_t markers = 0;  // flat markers placed in the world since the last heartbeat
    uint32_t lockOns = 0;  // frames the game placed its lock-on sight since then
    uint32_t hudPieces = 0;  // polygons kept on the HUD as fixed in front of the game's camera
    uint32_t boxesInView = 0, boxesOutOfView = 0;  // BOX_TEST answers the camera's view gave
};
extern GxWorldStats gWorldStats;
// BOX_TEST answers "visible" always (Platform::SetDrawAll).
extern std::atomic<bool> gDrawAll;
// The field's cursor is on a target at this guest VecFx32 this frame (khvr_host_lock_on): its
// kind (1, 3 enemies, 2 the field's entries) and whether it is locked on.
void GxNoteLockOn(uint32_t position, int kind, bool locked);
// Polygons in the list GxRender draws (for the heartbeat and tests).
uint32_t GxPolygonsShown();

}  // namespace khvr::hle
