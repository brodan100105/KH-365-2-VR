// The DS's two 2D engines, rendered in software from the guest's registers, palettes, OAM and
// VRAM: text, affine, extended and bitmap backgrounds, sprites (regular and affine), extended
// palettes, windows, alpha and brightness blending, master brightness.
//
// Engine A's BG0 can show the 3D engine's output. The VR layer draws the game's 3D itself, so
// that layer comes out transparent: `hud` gets engine A with a transparent backdrop too, for the
// floating HUD panel over the VR view.
#pragma once

#include <cstdint>

#include "khvr/platform.h"

namespace khvr::hle {

constexpr int kScreenW = PlatformScreens::kWidth, kScreenH = PlatformScreens::kHeight;

// `scale3d` > 1: the top screen's 3D layer is drawn at that many times the DS's resolution too,
// into out->topHi (out->topScale says whether it was).
void Render2D(PlatformScreens *out, int scale3d = 1);

}  // namespace khvr::hle
