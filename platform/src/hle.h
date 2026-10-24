// Shared state of the platform layer's DS hardware stand-ins (HLE = high-level emulation: the
// hardware's behaviour as the game's code sees it, not its circuits).
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

namespace khvr::hle {

// ---- Logging (platform/src/log.cpp): to the boot log file and the platform's ring buffer. ----
void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void LogOnce(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// The calling thread's native call chain (the game's, for guest code) into the log.
void LogBacktrace(const char *why);
// The calling thread's return addresses (safe in a signal handler), and their log formatting.
int CaptureBacktrace(void **pcs, int max);
void LogFrames(const char *why, void *const *pcs, int n, int skip);
// The game cannot go on: record why (platform.cpp).
void PlatformReportStop(const std::string &why);
const char *LastTrapName();  // what the calling thread's last wasm trap was

// ---- Interrupts (hle_thread.cpp) ----
enum : uint32_t {
    kIrqVBlank = 1u << 0,
    kIrqTimer0 = 1u << 3,
    kIrqDma0 = 1u << 8,
    kIrqIpcSync = 1u << 16,
    kIrqIpcRecv = 1u << 18,
    kIrqGxFifo = 1u << 21,
};
void RaiseIrq(uint32_t bits);
// Runs the guest's handlers for pending, enabled interrupts on the calling guest thread, as the
// SDK's OS_IrqHandler would. Safe to call whenever the guest has interrupts enabled.
void DispatchIrqs();
// The guest's CPSR I bit (kept by guest/runtime/os.c).
bool GuestIrqsDisabled();

// ---- I/O registers (hle_io.cpp) ----
uint64_t IoRead(uint32_t addr, uint32_t size);
void IoWrite(uint32_t addr, uint64_t value, uint32_t size);
void IoReset();
uint16_t &RegIme();
uint32_t &RegIe();
uint32_t &RegIf();
// Called once per emulated frame: line counter, timers.
void IoVBlank(uint64_t frameCycles);
void IoOnIrqTaken(int bit);  // re-raises timers with more overflows queued
uint8_t IoReg8(uint32_t off);  // plain register storage, for the renderers (offset from 0x04000000)
uint16_t IoReg16(uint32_t off);
uint32_t IoReg32(uint32_t off);
uint64_t IoCycles();  // emulated ARM9 bus cycles (33.5 MHz) since boot

// The VRAM banks A-I and how the guest has mapped them (VRAMCNT).
uint8_t *VramBank(int bank);  // 0 = A .. 8 = I
uint32_t VramBankSize(int bank);
// Copies `size` bytes of a CPU-visible VRAM region (BG/OBJ/LCDC) as currently mapped.
void VramCopyRegion(uint32_t base, uint32_t size, uint8_t *dst);
// Extended palette slot `slot` (BG 0-3, OBJ 0) of engine 0 (A) or 1 (B), or null if unmapped.
const uint8_t *VramExtPalette(int engine, bool obj, int slot);
// Plain memory reads of the 2D engines' regions through the current mapping.
uint8_t VramRead8(uint32_t addr);
uint16_t VramRead16(uint32_t addr);

// ---- The ARM7 (hle_arm7.cpp): answers the ARM9's PXI messages. ----
void Arm7Reset();
void Arm7OnSend(uint32_t word);   // a word the ARM9 wrote to IPCFIFOSEND
bool Arm7HasReply();
uint32_t Arm7PopReply();
uint32_t Arm7SyncOut(uint32_t arm9Out);  // IPCSYNC: the ARM7's nibble for the ARM9's
void Arm7VBlank();                       // per-frame work (touch sampling, sound tags)

// ---- ROM, save and boot environment (hle_rom.cpp) ----
bool RomOpen(const std::string &path, std::string *error);
uint32_t RomSize();
void RomRead(uint32_t offset, void *dst, uint32_t len);
void SetupSharedArea(int language);    // header copies, owner info, system work
bool SaveOpen(const std::string &path);
void SaveRead(uint32_t offset, void *dst, uint32_t len);
void SaveWrite(uint32_t offset, const void *src, uint32_t len);
bool SaveVerify(uint32_t offset, const void *src, uint32_t len);
void SaveErase(uint32_t offset, uint32_t len);
void SaveFlush();
void OverlaysInit();                   // snapshots every overlay's initialized data
void OverlayStart(uint32_t id);        // FS_StartOverlay: fresh .bss and .data for overlay `id`

// ---- Input (set by the app, read by the I/O and ARM7 stand-ins) ----
struct InputState {
    uint16_t keys = 0;      // DS buttons, active high (DsButton bits)
    bool touching = false;
    uint16_t touchX = 0, touchY = 0;
};
InputState &Input();

}  // namespace khvr::hle
