#include "hle_gx.h"

namespace khvr::hle {

namespace {
uint32_t gCommands = 0, gLastFrame = 0;
}

void GxWrite(uint32_t addr, uint32_t value, uint32_t size) {
    (void)addr;
    (void)value;
    (void)size;
    ++gCommands;
}

uint16_t GxReadRegister(uint32_t off) {
    (void)off;
    return 0;  // clip / vector matrix and test results: filled in by the geometry engine later
}

uint32_t GxCommandsThisFrame() { return gLastFrame; }

void GxVBlank() {
    gLastFrame = gCommands;
    gCommands = 0;
}

}  // namespace khvr::hle
