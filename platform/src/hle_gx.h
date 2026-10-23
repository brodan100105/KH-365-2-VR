// The 3D geometry engine. Iteration 2 only keeps the guest's view of it consistent (the FIFO
// always drains, status reads are idle); iteration 3 decodes the command stream into the
// world-space draw lists the VR renderer uses (docs/ARCHITECTURE.md).
#pragma once

#include <cstdint>

namespace khvr::hle {

void GxWrite(uint32_t addr, uint32_t value, uint32_t size);
uint16_t GxReadRegister(uint32_t off);  // 0x600-0x6ff, offset from 0x04000000
uint32_t GxCommandsThisFrame();
void GxVBlank();

}  // namespace khvr::hle
