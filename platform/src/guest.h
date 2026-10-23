// The guest: the decompiled game as translated by wasm2c (build/guest/wasm2c), and the helpers
// the platform layer uses to reach into it: its linear memory (the DS memory map), its exported
// symbols, and calls into its functions.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "khdays.h"
}

namespace khvr::hle {

// The single guest instance. wasm2c keeps every global and the memory in it.
w2c_khdays *Guest();

inline uint8_t *GuestMem() { return Guest()->w2c_memory.data; }
inline uint64_t GuestMemSize() { return Guest()->w2c_memory.size; }

// Plain memory, bypassing the I/O traps (the platform's own view of RAM).
inline uint8_t Read8(uint32_t a) { return GuestMem()[a]; }
inline uint16_t Read16(uint32_t a) { uint16_t v; std::memcpy(&v, GuestMem() + a, 2); return v; }
inline uint32_t Read32(uint32_t a) { uint32_t v; std::memcpy(&v, GuestMem() + a, 4); return v; }
inline void Write8(uint32_t a, uint8_t v) { GuestMem()[a] = v; }
inline void Write16(uint32_t a, uint16_t v) { std::memcpy(GuestMem() + a, &v, 2); }
inline void Write32(uint32_t a, uint32_t v) { std::memcpy(GuestMem() + a, &v, 4); }
inline void ReadBlock(uint32_t a, void *dst, size_t n) { std::memcpy(dst, GuestMem() + a, n); }
inline void WriteBlock(uint32_t a, const void *src, size_t n) { std::memcpy(GuestMem() + a, src, n); }
std::string ReadString(uint32_t a, size_t max = 256);

// Bus accesses as the guest's own loads and stores see them: I/O and VRAM go through the traps.
uint32_t BusRead(uint32_t a, uint32_t size);
void BusWrite(uint32_t a, uint32_t v, uint32_t size);

// Address of an exported guest symbol (function table index for functions), 0 if unknown.
uint32_t SymbolAddress(const char *name);

// Calls guest function `index` (a function pointer value) with up to four integer arguments, the
// way an ARM `blx` would: missing parameters read as 0, extra ones are ignored.
uint32_t CallGuest(uint32_t index, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0);

}  // namespace khvr::hle
