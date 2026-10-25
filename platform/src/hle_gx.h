// The 3D engine: the geometry engine the guest feeds through the GX FIFO and command ports
// (matrices, lighting, texture coordinates, primitives, clipping, the box/position/vector tests)
// and a software rasterizer that draws the polygons of the last SWAP_BUFFERS the way the DS's
// rendering engine does (textures in all seven formats, modulate/decal/toon/highlight, alpha
// test and blending, depth or W buffer, fog). Its picture is engine A's BG0 when DISPCNT puts
// the 3D layer there (render2d.cpp).
//
// The VR view will later take the same polygons as world-space draw lists (docs/ROADMAP.md).
#pragma once

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
void GxRender(uint16_t *color, uint8_t *alpha, int scale = 1);
// The polygons of the last SWAP_BUFFERS drawn with the game's main camera, in world space.
void GxCaptureWorld(PlatformWorld *out);
// Polygons in the list GxRender draws (for the heartbeat and tests).
uint32_t GxPolygonsShown();

}  // namespace khvr::hle
