// The ARM9's I/O registers (0x04000000-0x04001fff, the IPC receive port at 0x04100000) and VRAM
// (0x06000000-0x06ffffff), as the guest's trapped loads and stores see them
// (guest/postprocess.py routes every access there).
//
// Most registers are plain storage that the 2D renderer and the 3D capture read back. Registers
// with behaviour: DISPSTAT/VCOUNT (a line counter driven by emulated time), timers, DMA (done at
// once), the divider and square root unit, IPC (to the ARM7 stand-in), the keypad, interrupts,
// VRAM bank mapping, and the geometry engine's status.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

#include "guest.h"
#include "hle.h"
#include "hle_gx.h"

namespace khvr::hle {

namespace {

constexpr uint32_t kIoBase = 0x04000000, kIoSize = 0x2000;
constexpr uint64_t kCyclesPerLine = 2130;   // 355 dots x 6 cycles at 33.51 MHz
constexpr uint32_t kLines = 263;
// Each I/O read advances emulated time a little, so that code polling a register (VCOUNT, a
// timer) sees it move even between VBlanks.
constexpr uint64_t kCyclesPerIoRead = 16;

alignas(4) std::array<uint8_t, kIoSize> gRegs;
uint32_t gIf = 0;
uint64_t gCycles = 0;

uint16_t R16(uint32_t off) { uint16_t v; std::memcpy(&v, &gRegs[off], 2); return v; }
uint32_t R32(uint32_t off) { uint32_t v; std::memcpy(&v, &gRegs[off], 4); return v; }
void W16(uint32_t off, uint16_t v) { std::memcpy(&gRegs[off], &v, 2); }
void W32(uint32_t off, uint32_t v) { std::memcpy(&gRegs[off], &v, 4); }

// ---------------------------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------------------------
struct Timer {
    uint16_t reload = 0;
    uint16_t ctrl = 0;
    uint64_t start = 0;       // cycle the timer started counting
    uint64_t overflowsSeen = 0;
    uint32_t pendingIrqs = 0;
};
Timer gTimers[4];
const int kPrescalerShift[4] = {0, 6, 8, 10};

uint64_t TimerTicks(const Timer &t) { return (gCycles - t.start) >> kPrescalerShift[t.ctrl & 3]; }

uint16_t TimerCounter(const Timer &t) {
    if (!(t.ctrl & 0x80)) return t.reload;
    uint64_t period = 0x10000u - t.reload;
    return static_cast<uint16_t>(t.reload + TimerTicks(t) % period);
}

void TimersUpdate() {
    for (int i = 0; i < 4; ++i) {
        Timer &t = gTimers[i];
        if (!(t.ctrl & 0x80) || (t.ctrl & 4)) continue;  // stopped, or counting up (unused)
        uint64_t period = 0x10000u - t.reload;
        uint64_t overflows = TimerTicks(t) / period;
        if (overflows > t.overflowsSeen) {
            if (t.ctrl & 0x40) {
                // Deliver every overflow (the OS tick counts them), at most a frame's worth.
                uint64_t n = overflows - t.overflowsSeen;
                t.pendingIrqs += static_cast<uint32_t>(n > 64 ? 64 : n);
                gIf |= kIrqTimer0 << i;
            }
            t.overflowsSeen = overflows;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Divider and square root
// ---------------------------------------------------------------------------------------------
void Divide() {
    uint16_t cnt = R16(0x280);
    int64_t num, den;
    std::memcpy(&num, &gRegs[0x290], 8);
    std::memcpy(&den, &gRegs[0x298], 8);
    switch (cnt & 3) {
    case 0: num = static_cast<int32_t>(num); den = static_cast<int32_t>(den); break;
    case 1: den = static_cast<int32_t>(den); break;
    default: break;
    }
    int64_t q, r;
    if (den == 0) {
        q = num < 0 ? 1 : -1;  // the hardware's answer, and its divide-by-zero flag
        r = num;
        cnt |= 0x4000;
    } else if (num == INT64_MIN && den == -1) {
        q = num;
        r = 0;
        cnt &= ~0x4000;
    } else {
        q = num / den;
        r = num % den;
        cnt &= ~0x4000;
    }
    std::memcpy(&gRegs[0x2a0], &q, 8);
    std::memcpy(&gRegs[0x2a8], &r, 8);
    W16(0x280, cnt);
}

void SquareRoot() {
    uint64_t v;
    std::memcpy(&v, &gRegs[0x2b8], 8);
    if (!(R16(0x2b0) & 1)) v &= 0xffffffffu;
    uint64_t r = static_cast<uint64_t>(std::sqrt(static_cast<long double>(v)));
    while (r * r > v) --r;
    while ((r + 1) * (r + 1) <= v) ++r;
    W32(0x2b4, static_cast<uint32_t>(r));
}

// ---------------------------------------------------------------------------------------------
// DMA: every transfer happens the moment it is enabled.
// ---------------------------------------------------------------------------------------------
void RunDma(int ch) {
    uint32_t base = 0xb0 + 12 * ch;
    uint32_t src = R32(base), dst = R32(base + 4), cnt = R32(base + 8);
    uint32_t count = cnt & 0x1fffff;
    if (count == 0) count = 0x200000;
    uint32_t unit = (cnt & (1u << 26)) ? 4 : 2;
    int srcStep = 0, dstStep = 0;
    switch ((cnt >> 23) & 3) { case 0: srcStep = unit; break; case 1: srcStep = -static_cast<int>(unit); break; default: break; }
    switch ((cnt >> 21) & 3) { case 0: case 3: dstStep = unit; break; case 1: dstStep = -static_cast<int>(unit); break; default: break; }
    uint32_t timing = (cnt >> 27) & 7;
    if (timing == 5) {
        LogOnce("dma-card", "dma: card-timed DMA (ROM reads go through CARDi_ReadRom instead)");
    }
    WatchHostWrite(dstStep < 0 ? dst - (count - 1) * unit : dst, count * unit, "DMA");
    // A DMA "fill" reads its source from the channel's fill register.
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = BusRead(src, unit);
        BusWrite(dst, v, unit);
        src += srcStep;
        dst += dstStep;
    }
    if (((cnt >> 23) & 3) != 2) W32(base, src);
    if (((cnt >> 21) & 3) != 3 && ((cnt >> 21) & 3) != 2) W32(base + 4, dst);
    if (!(cnt & (1u << 25)) || timing == 0) W32(base + 8, cnt & ~(1u << 31));
    if (cnt & (1u << 30)) gIf |= kIrqDma0 << ch;
}

// ---------------------------------------------------------------------------------------------
// VRAM banks and their mapping
// ---------------------------------------------------------------------------------------------
const uint32_t kBankSize[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};
const uint32_t kLcdcBase[9] = {0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000,
                               0x06890000, 0x06894000, 0x06898000, 0x068a0000};
std::vector<uint8_t> gBanks[9];

constexpr uint32_t kPage = 0x4000;  // 16 KB pages
struct Mapping {
    int8_t bank = -1;
    uint32_t offset = 0;
};
// CPU-visible regions: BG-A 0x06000000 (512K), BG-B 0x06200000 (128K), OBJ-A 0x06400000 (256K),
// OBJ-B 0x06600000 (128K), LCDC 0x06800000 (656K). Up to three banks can overlap in a page.
struct Region {
    uint32_t base, size;
    std::vector<std::array<Mapping, 3>> pages;
};
Region gRegions[5] = {{0x06000000, 0x80000, {}}, {0x06200000, 0x20000, {}}, {0x06400000, 0x40000, {}},
                      {0x06600000, 0x20000, {}}, {0x06800000, 0xa4000, {}}};

void MapRange(int region, uint32_t offset, int bank, uint32_t size) {
    Region &r = gRegions[region];
    for (uint32_t o = 0; o < size; o += kPage) {
        uint32_t page = (offset + o) / kPage;
        if (page >= r.pages.size()) break;
        for (Mapping &m : r.pages[page])
            if (m.bank < 0) {
                m.bank = static_cast<int8_t>(bank);
                m.offset = o;
                break;
            }
    }
}

void Remap() {
    for (Region &r : gRegions) r.pages.assign(r.size / kPage, {});
    for (int b = 0; b < 9; ++b) {
        uint8_t cnt = gRegs[0x240 + b + (b >= 7 ? 1 : 0)];  // VRAMCNT_H/I sit at 0x248/0x249
        if (!(cnt & 0x80)) continue;
        uint32_t mst = cnt & 7, ofs = (cnt >> 3) & 3;
        uint32_t size = kBankSize[b];
        switch (mst) {
        case 0: MapRange(4, kLcdcBase[b] - 0x06800000, b, size); break;
        case 1:
            if (b <= 3) MapRange(0, 0x20000 * ofs, b, size);
            else if (b == 4) MapRange(0, 0, b, size);
            else if (b <= 6) MapRange(0, 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1), b, size);
            else if (b == 7) MapRange(1, 0, b, size);
            else MapRange(1, 0x8000, b, size);
            break;
        case 2:
            if (b <= 1) MapRange(2, 0x20000 * (ofs & 1), b, size);
            else if (b == 4) MapRange(2, 0, b, size);
            else if (b == 5 || b == 6) MapRange(2, 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1), b, size);
            else if (b == 8) MapRange(3, 0, b, size);
            break;  // C/D in mode 2 belong to the ARM7; H in mode 2 is BG ext palette B
        case 4:
            if (b == 2) MapRange(1, 0, b, size);
            else if (b == 3) MapRange(3, 0, b, size);
            break;
        default: break;  // texture image / palette and extended palette slots: not CPU-visible
        }
    }
}

bool Locate(uint32_t addr, Region **region, uint32_t *off) {
    for (Region &r : gRegions) {
        // The 2D regions repeat through their 2 MB windows; LCDC runs to the end of VRAM.
        uint32_t span = r.base == 0x06800000 ? 0x800000 : 0x200000;
        if (addr >= r.base && addr < r.base + span) {
            *region = &r;
            *off = (addr - r.base) % r.size;  // the 2D regions mirror
            return true;
        }
    }
    return false;
}

uint8_t VramByte(uint32_t addr) {
    Region *r;
    uint32_t off;
    if (!Locate(addr, &r, &off)) return 0;
    uint8_t v = 0;
    for (const Mapping &m : r->pages[off / kPage])
        if (m.bank >= 0) v |= gBanks[m.bank][m.offset + off % kPage];
    return v;
}

void VramWriteByte(uint32_t addr, uint8_t v) {
    Region *r;
    uint32_t off;
    if (!Locate(addr, &r, &off)) return;
    for (const Mapping &m : r->pages[off / kPage])
        if (m.bank >= 0) gBanks[m.bank][m.offset + off % kPage] = v;
}

// ---------------------------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------------------------
uint32_t gLastRecv = 0;

uint16_t IpcFifoCnt() {
    uint16_t cnt = R16(0x184) & 0x8404;  // enable, recv-irq enable, send-irq enable
    cnt |= 1;                            // send FIFO always empty: the ARM7 takes words at once
    if (!Arm7HasReply()) cnt |= 0x100;   // receive FIFO empty
    return cnt;
}

void IpcAfterSend() {
    if (Arm7HasReply() && (R16(0x184) & 0x400)) gIf |= kIrqIpcRecv;
}

// ---------------------------------------------------------------------------------------------
// Synthesized registers
// ---------------------------------------------------------------------------------------------
uint32_t Line() { return static_cast<uint32_t>((gCycles / kCyclesPerLine) % kLines); }

uint16_t Read16Reg(uint32_t off) {
    switch (off) {
    case 0x004: {  // DISPSTAT
        uint32_t line = Line();
        uint16_t v = R16(0x004) & 0xffb8;
        if (line >= 192 && line < 262) v |= 1;
        if ((gCycles % kCyclesPerLine) > 1536) v |= 2;
        uint32_t match = (v >> 8) | ((v & 0x80) << 1);
        if (line == match) v |= 4;
        return v;
    }
    case 0x006: return static_cast<uint16_t>(Line());
    case 0x100: case 0x104: case 0x108: case 0x10c: return TimerCounter(gTimers[(off - 0x100) / 4]);
    case 0x102: case 0x106: case 0x10a: case 0x10e: return gTimers[(off - 0x102) / 4].ctrl;
    case 0x130: return static_cast<uint16_t>(~Input().keys & 0x3ff);  // KEYINPUT, active low
    case 0x180: {  // IPCSYNC: our output bits 8-11, the ARM7's in bits 0-3
        uint16_t v = R16(0x180) & 0x4f00;
        return static_cast<uint16_t>(v | Arm7SyncOut((v >> 8) & 0xf));
    }
    case 0x184: return IpcFifoCnt();
    case 0x214: return static_cast<uint16_t>(gIf);
    case 0x216: return static_cast<uint16_t>(gIf >> 16);
    case 0x600: return 0;                                  // GXSTAT low: nothing busy, no tests
    case 0x602: return static_cast<uint16_t>(0x0600 | (R16(0x602) & 0xc000));  // FIFO empty
    default:
        if (off >= 0x600 && off < 0x700) return GxReadRegister(off);
        return R16(off);
    }
}

void OnWrite16(uint32_t off, uint16_t old) {
    switch (off) {
    case 0x0ba: case 0x0c6: case 0x0d2: case 0x0de:  // DMAxCNT high half
        if ((R16(off) & 0x8000) && !(old & 0x8000)) RunDma((off - 0xba) / 12);
        break;
    case 0x102: case 0x106: case 0x10a: case 0x10e: {
        Timer &t = gTimers[(off - 0x102) / 4];
        uint16_t ctrl = R16(off);
        if ((ctrl & 0x80) && !(t.ctrl & 0x80)) {
            t.start = gCycles;
            t.overflowsSeen = 0;
            t.pendingIrqs = 0;
        }
        t.ctrl = ctrl;
        break;
    }
    case 0x100: case 0x104: case 0x108: case 0x10c:
        gTimers[(off - 0x100) / 4].reload = R16(off);
        break;
    case 0x180:
        if (R16(0x180) & 0x2000) LogOnce("ipcsync-irq", "ipc: IPCSYNC interrupt to the ARM7 (ignored)");
        break;
    case 0x184: {
        uint16_t v = R16(0x184);
        if (v & 0x4000) v &= ~0x4000;  // acknowledge an error
        W16(0x184, v);
        break;
    }
    case 0x280: case 0x290: case 0x292: case 0x294: case 0x296:
    case 0x298: case 0x29a: case 0x29c: case 0x29e:
        Divide();
        break;
    case 0x2b0: case 0x2b8: case 0x2ba: case 0x2bc: case 0x2be:
        SquareRoot();
        break;
    case 0x240: case 0x242: case 0x244: case 0x246: case 0x248:
        Remap();
        break;
    default: break;
    }
}

}  // namespace

uint16_t &RegIme() { return *reinterpret_cast<uint16_t *>(&gRegs[0x208]); }
uint32_t &RegIe() { return *reinterpret_cast<uint32_t *>(&gRegs[0x210]); }
uint32_t &RegIf() { return gIf; }

uint64_t IoCycles() { return gCycles; }

void IoReset() {
    gRegs.fill(0);
    gIf = 0;
    gCycles = 0;
    for (Timer &t : gTimers) t = Timer();
    for (int b = 0; b < 9; ++b) gBanks[b].assign(kBankSize[b], 0);
    Remap();
}

void IoVBlank(uint64_t frameCycles) {
    // Jump to the start of the next frame's VBlank (line 192).
    uint64_t frame = kCyclesPerLine * kLines;
    uint64_t next = (gCycles / frame + 1) * frame + 192 * kCyclesPerLine;
    gCycles = next > gCycles + frameCycles ? next : gCycles + frameCycles;
    TimersUpdate();
    gIf |= kIrqVBlank;
}

// Called by DispatchIrqs after it took an interrupt: timers with more overflows queued raise
// theirs again, so the OS tick counts every one.
void IoOnIrqTaken(int bit) {
    if (bit >= 3 && bit <= 6) {
        Timer &t = gTimers[bit - 3];
        if (t.pendingIrqs > 0 && --t.pendingIrqs > 0) gIf |= 1u << bit;
    }
}

uint64_t IoRead(uint32_t addr, uint32_t size) {
    if ((addr >> 24) == 6) {
        uint64_t v = 0;
        for (uint32_t i = 0; i < size; ++i) v |= static_cast<uint64_t>(VramByte(addr + i)) << (8 * i);
        return v;
    }
    gCycles += kCyclesPerIoRead;
    if (addr == 0x04100000) {
        if (Arm7HasReply()) gLastRecv = Arm7PopReply();
        return gLastRecv;
    }
    if (addr == 0x04100010) return 0;  // card data port: ROM reads bypass the card bus
    uint32_t off = addr - kIoBase;
    if (off >= kIoSize) {
        LogOnce("io-read-high", "io: read of unmapped register 0x%08x", addr);
        return 0;
    }
    if ((off >= 0x100 && off < 0x110) || off == 0x004 || off == 0x006) TimersUpdate();
    uint64_t v = 0;
    uint32_t first = off & ~1u;
    for (uint32_t o = first; o < off + size; o += 2) {
        uint16_t h = Read16Reg(o);
        int shift = static_cast<int>(o) - static_cast<int>(off);
        v |= shift >= 0 ? static_cast<uint64_t>(h) << (8 * shift) : static_cast<uint64_t>(h >> 8);
    }
    return size == 8 ? v : (v & ((1ull << (8 * size)) - 1));
}

void IoWrite(uint32_t addr, uint64_t value, uint32_t size) {
    if ((addr >> 24) == 6) {
        for (uint32_t i = 0; i < size; ++i) VramWriteByte(addr + i, static_cast<uint8_t>(value >> (8 * i)));
        return;
    }
    uint32_t off = addr - kIoBase;
    if (off >= kIoSize) {
        LogOnce("io-write-high", "io: write to unmapped register 0x%08x", addr);
        return;
    }
    if (off == 0x188) {  // IPCFIFOSEND
        Arm7OnSend(static_cast<uint32_t>(value));
        IpcAfterSend();
        return;
    }
    if (off >= 0x214 && off < 0x218) {  // IF: writing 1 acknowledges
        uint32_t mask = static_cast<uint32_t>(value << (8 * (off - 0x214)));
        gIf &= ~mask;
        return;
    }
    if (off >= 0x400 && off < 0x600) {  // geometry engine
        GxWrite(addr, static_cast<uint32_t>(value), size);
        return;
    }
    uint16_t oldHalves[5];
    uint32_t first = off & ~1u;
    for (uint32_t o = first, i = 0; o < off + size && i < 5; o += 2, ++i) oldHalves[i] = R16(o);
    for (uint32_t i = 0; i < size; ++i) gRegs[off + i] = static_cast<uint8_t>(value >> (8 * i));
    for (uint32_t o = first, i = 0; o < off + size && i < 5; o += 2, ++i) OnWrite16(o, oldHalves[i]);
}

void VramCopyRegion(uint32_t base, uint32_t size, uint8_t *dst) {
    Region *r;
    uint32_t off;
    if (!Locate(base, &r, &off)) {
        std::memset(dst, 0, size);
        return;
    }
    for (uint32_t done = 0; done < size; done += kPage) {
        uint32_t o = (off + done) % r->size;
        uint32_t n = std::min<uint32_t>(kPage, size - done);
        const auto &maps = r->pages[o / kPage];
        int used = 0;
        for (const Mapping &m : maps) {
            if (m.bank < 0) continue;
            const uint8_t *src = &gBanks[m.bank][m.offset];
            if (used++ == 0) std::memcpy(dst + done, src, n);
            else for (uint32_t i = 0; i < n; ++i) dst[done + i] |= src[i];
        }
        if (!used) std::memset(dst + done, 0, n);
    }
}

const uint8_t *VramExtPalette(int engine, bool obj, int slot) {
    for (int b = 4; b < 9; ++b) {
        uint8_t cnt = gRegs[0x240 + b + (b >= 7 ? 1 : 0)];
        if (!(cnt & 0x80)) continue;
        uint32_t mst = cnt & 7, ofs = (cnt >> 3) & 3;
        const uint8_t *bank = gBanks[b].data();
        if (engine == 0 && !obj) {
            if (b == 4 && mst == 4) return bank + 0x2000 * slot;
            if ((b == 5 || b == 6) && mst == 4 && static_cast<uint32_t>(slot / 2) == (ofs & 1)) return bank + 0x2000 * (slot & 1);
        } else if (engine == 0 && obj) {
            if ((b == 5 || b == 6) && mst == 5) return bank;
        } else if (engine == 1 && !obj) {
            if (b == 7 && mst == 2) return bank + 0x2000 * slot;
        } else {
            if (b == 8 && mst == 3) return bank;
        }
    }
    return nullptr;
}

uint8_t *VramBank(int bank) { return gBanks[bank].data(); }
uint32_t VramBankSize(int bank) { return kBankSize[bank]; }
uint8_t VramRead8(uint32_t addr) { return VramByte(addr); }
uint16_t VramRead16(uint32_t addr) { return static_cast<uint16_t>(VramByte(addr) | (VramByte(addr + 1) << 8)); }
uint8_t IoReg8(uint32_t off) { return gRegs[off]; }
uint16_t IoReg16(uint32_t off) { return R16(off); }
uint32_t IoReg32(uint32_t off) { return R32(off); }

}  // namespace khvr::hle

// Guest imports for the trapped accesses (guest/postprocess.py).
extern "C" uint64_t khvr_io_read(uint32_t addr, uint32_t size) { return khvr::hle::IoRead(addr, size); }
extern "C" void khvr_io_write(uint32_t addr, uint64_t value, uint32_t size) { khvr::hle::IoWrite(addr, value, size); }

// ---------------------------------------------------------------------------------------------
// Write watch: logs the first stores into a few bytes of guest data, with the host call stack
// (guest functions show as w2c_khdays_<name>), to find what overwrites them. The guest's stores
// check the range (guest/postprocess.py); the platform's own writes call WatchHostWrite.
// ---------------------------------------------------------------------------------------------
extern "C" {
uint32_t khvr_watch_lo = 0, khvr_watch_len = 0;
}

namespace khvr::hle {
namespace {
uint32_t gWatchStart = 0, gWatchSize = 0;
int gWatchHits = 0;
constexpr int kMaxWatchHits = 10;
}  // namespace

void WatchSet(uint32_t addr, uint32_t size, const char *what) {
    gWatchStart = addr;
    gWatchSize = size;
    // A store of up to 8 bytes that starts just before the range still overlaps it.
    khvr_watch_lo = addr - 7;
    khvr_watch_len = size + 7;
    Log("watch: %u bytes at 0x%08x (%s)", size, addr, what);
}

void WatchHostWrite(uint32_t addr, uint32_t len, const char *what) {
    if (!gWatchSize || gWatchHits >= kMaxWatchHits) return;
    if (addr >= gWatchStart + gWatchSize || addr + len <= gWatchStart) return;
    ++gWatchHits;
    Log("watch: %s wrote 0x%x bytes at 0x%08x, over the watched bytes", what, len, addr);
    LogBacktrace("watch");
}
}  // namespace khvr::hle

extern "C" void khvr_watch_hit(uint32_t addr, uint64_t value, uint32_t size) {
    using namespace khvr::hle;
    if (addr >= gWatchStart + gWatchSize || addr + size <= gWatchStart || gWatchHits >= kMaxWatchHits) return;
    ++gWatchHits;
    Log("watch: guest stored 0x%llx (%u bytes) at 0x%08x", static_cast<unsigned long long>(value), size, addr);
    LogBacktrace("watch");
}
