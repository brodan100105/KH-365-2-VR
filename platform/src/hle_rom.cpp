// The ROM image, the save file, the boot environment the DS's firmware leaves in the shared
// area, and overlay resets.
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "guest.h"
#include "hle.h"

extern "C" {
struct khvr_overlay_range {
    uint16_t id;
    uint8_t zero;
    uint32_t addr, size;
};
extern const struct khvr_overlay_range khvr_overlay_ranges[];
extern const unsigned khvr_overlay_range_count;
}

namespace khvr::hle {

namespace {

const uint8_t *gRom = nullptr;
size_t gRomSize = 0;

std::string gSavePath;
std::vector<uint8_t> gSave;
bool gSaveDirty = false;

std::vector<std::vector<uint8_t>> gOverlaySnapshots;  // per range, initialized data at boot

}  // namespace

bool RomOpen(const std::string &path, std::string *error) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        *error = "cannot open " + path;
        return false;
    }
    struct stat st;
    fstat(fd, &st);
    void *p = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED || st.st_size < 0x4000) {
        *error = "cannot map " + path;
        return false;
    }
    gRom = static_cast<const uint8_t *>(p);
    gRomSize = static_cast<size_t>(st.st_size);
    char code[5] = {0};
    std::memcpy(code, gRom + 0x0c, 4);
    Log("rom: %s, %zu MB, game code %s", path.c_str(), gRomSize >> 20, code);
    if (std::strcmp(code, "YKGP") != 0) {
        *error = std::string("this is not the European release of 358/2 Days (game code ") + code +
                 ", expected YKGP); the decompilation only matches that one";
        return false;
    }
    return true;
}

uint32_t RomSize() { return static_cast<uint32_t>(gRomSize); }

namespace {
uint32_t RomU32(size_t off) { return off + 4 <= gRomSize ? gRom[off] | gRom[off + 1] << 8 | gRom[off + 2] << 16 | static_cast<uint32_t>(gRom[off + 3]) << 24 : 0; }
uint16_t RomU16(size_t off) { return off + 2 <= gRomSize ? static_cast<uint16_t>(gRom[off] | gRom[off + 1] << 8) : 0; }

// One NitroFS directory of the file name table, recursively: "path size" per file.
void ListDir(FILE *out, uint32_t fnt, uint32_t fat, uint16_t dir, const std::string &prefix, int depth) {
    if (depth > 16) return;
    size_t entry = fnt + (dir & 0x0fff) * 8u;
    size_t p = fnt + RomU32(entry);
    uint16_t fileId = RomU16(entry + 4);
    while (p < gRomSize) {
        uint8_t len = gRom[p++];
        if (len == 0) break;
        std::string name(reinterpret_cast<const char *>(gRom + p), std::min<size_t>(len & 0x7f, gRomSize - p));
        p += len & 0x7f;
        if (len & 0x80) {
            uint16_t sub = RomU16(p);
            p += 2;
            ListDir(out, fnt, fat, sub, prefix + name + "/", depth + 1);
        } else {
            uint32_t start = RomU32(fat + fileId * 8u), end = RomU32(fat + fileId * 8u + 4);
            std::fprintf(out, "%s%s %u\n", prefix.c_str(), name.c_str(), end > start ? end - start : 0);
            ++fileId;
        }
    }
}
}  // namespace

namespace {
bool SameNameNoCase(const uint8_t *a, size_t n, const std::string &b) {
    if (n != b.size()) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(a[i]) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
}  // namespace

const uint8_t *RomFindFile(const std::string &path, uint32_t *size) {
    if (!gRom) return nullptr;
    uint32_t fnt = RomU32(0x40), fat = RomU32(0x48);
    uint16_t dir = 0xf000;
    size_t at = 0;
    while (at < path.size() && path[at] == '/') ++at;
    while (at < path.size()) {
        size_t slash = path.find('/', at);
        bool last = slash == std::string::npos;
        std::string part = path.substr(at, last ? std::string::npos : slash - at);
        at = last ? path.size() : slash + 1;
        size_t entry = fnt + (dir & 0x0fff) * 8u;
        size_t p = fnt + RomU32(entry);
        uint16_t fileId = RomU16(entry + 4);
        bool found = false;
        while (p < gRomSize && !found) {
            uint8_t len = gRom[p++];
            if (len == 0) break;
            size_t n = std::min<size_t>(len & 0x7f, gRomSize - p);
            bool match = SameNameNoCase(gRom + p, n, part);
            p += len & 0x7f;
            if (len & 0x80) {
                uint16_t sub = RomU16(p);
                p += 2;
                if (match && !last) dir = sub, found = true;
            } else {
                if (match && last) {
                    uint32_t start = RomU32(fat + fileId * 8u), end = RomU32(fat + fileId * 8u + 4);
                    if (end < start || end > gRomSize) return nullptr;
                    *size = end - start;
                    return gRom + start;
                }
                ++fileId;
            }
        }
        if (!found) return nullptr;
    }
    return nullptr;
}

void RomWriteFileList(const std::string &path) {
    if (!gRom || path.empty()) return;
    FILE *out = std::fopen(path.c_str(), "w");
    if (!out) return;
    // Names and sizes only, from the file name table (header 0x40) and allocation table (0x48).
    ListDir(out, RomU32(0x40), RomU32(0x48), 0xf000, "/", 0);
    std::fclose(out);
    Log("rom: file list written to %s", path.c_str());
}

void RomRead(uint32_t offset, void *dst, uint32_t len) {
    if (offset >= gRomSize) {
        std::memset(dst, 0xff, len);
        return;
    }
    uint32_t n = static_cast<uint32_t>(std::min<size_t>(len, gRomSize - offset));
    std::memcpy(dst, gRom + offset, n);
    if (n < len) std::memset(static_cast<uint8_t *>(dst) + n, 0xff, len - n);
}

void SetupSharedArea(int language) {
    // The ROM header, where the firmware and the card library leave it.
    uint8_t header[0x160];
    RomRead(0, header, sizeof header);
    WriteBlock(0x027ffe00, header, sizeof header);  // HW_ROM_HEADER_BUF
    WriteBlock(0x027ffa80, header, sizeof header);  // HW_CARD_ROM_HEADER

    // The owner's profile from the firmware (OS_GetOwnerInfo reads 0x027ffc80).
    const uint32_t user = 0x027ffc80;
    for (uint32_t i = 0; i < 0x70; ++i) Write8(user + i, 0);
    Write8(user + 2, 11);          // favourite colour
    Write8(user + 3, 8);           // birthday month and day (any valid date)
    Write8(user + 4, 8);
    const char *nick = "Roxas";
    for (int i = 0; nick[i]; ++i) Write16(user + 6 + 2 * i, static_cast<uint16_t>(nick[i]));
    Write8(user + 0x1a, static_cast<uint8_t>(std::strlen(nick)));
    // Touch calibration: raw value = pixel x 16 (hle_arm7.cpp writes samples that way).
    Write16(user + 0x58, 16 * 16);
    Write16(user + 0x5a, 16 * 16);
    Write8(user + 0x5c, 16);
    Write8(user + 0x5d, 16);
    Write16(user + 0x5e, 240 * 16);
    Write16(user + 0x60, 176 * 16);
    Write8(user + 0x62, 240);
    Write8(user + 0x63, 176);
    // Language: 1 English, 2 French, 3 German, 4 Italian, 5 Spanish (Game > Language).
    Write16(user + 0x64, static_cast<uint16_t>(language & 7));

    Write16(0x027ffc40, 1);         // boot type: from the card (not download play)
    Write32(0x027fff8c, 0xffffffff);  // every ARM7 PXI callback is ready
    Write16(0x027fffa8, 0x2c00);    // X, Y, debug released; lid open
    Log("boot: shared area set up, language %d", language);
}

bool SaveOpen(const std::string &path) {
    gSavePath = path;
    gSave.clear();
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0) {
            gSave.resize(static_cast<size_t>(n));
            if (std::fread(gSave.data(), 1, gSave.size(), f) != gSave.size()) gSave.clear();
        }
        std::fclose(f);
        Log("save: %s, %zu KB", path.c_str(), gSave.size() >> 10);
    } else {
        Log("save: none yet at %s", path.c_str());
    }
    return true;
}

namespace {
void SaveGrow(size_t end) {
    if (end > gSave.size() && end <= 0x800000) gSave.resize(end, 0xff);
}
}  // namespace

void SaveRead(uint32_t offset, void *dst, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        size_t o = static_cast<size_t>(offset) + i;
        static_cast<uint8_t *>(dst)[i] = o < gSave.size() ? gSave[o] : 0xff;
    }
}

void SaveWrite(uint32_t offset, const void *src, uint32_t len) {
    SaveGrow(static_cast<size_t>(offset) + len);
    if (static_cast<size_t>(offset) + len > gSave.size()) return;
    std::memcpy(gSave.data() + offset, src, len);
    gSaveDirty = true;
}

bool SaveVerify(uint32_t offset, const void *src, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        size_t o = static_cast<size_t>(offset) + i;
        uint8_t v = o < gSave.size() ? gSave[o] : 0xff;
        if (v != static_cast<const uint8_t *>(src)[i]) return false;
    }
    return true;
}

void SaveErase(uint32_t offset, uint32_t len) {
    size_t end = std::min(gSave.size(), static_cast<size_t>(offset) + len);
    for (size_t o = offset; o < end; ++o) gSave[o] = 0xff;
    gSaveDirty = true;
}

void SaveFlush() {
    if (!gSaveDirty || gSavePath.empty()) return;
    std::string tmp = gSavePath + ".tmp";
    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
        bool ok = std::fwrite(gSave.data(), 1, gSave.size(), f) == gSave.size();
        ok = std::fclose(f) == 0 && ok;
        if (ok && std::rename(tmp.c_str(), gSavePath.c_str()) == 0) {
            gSaveDirty = false;
            Log("save: wrote %zu KB", gSave.size() >> 10);
        }
    }
}

void OverlaysInit() {
    gOverlaySnapshots.assign(khvr_overlay_range_count, {});
    for (unsigned i = 0; i < khvr_overlay_range_count; ++i) {
        const auto &r = khvr_overlay_ranges[i];
        if (!r.zero) {
            gOverlaySnapshots[i].resize(r.size);
            ReadBlock(r.addr, gOverlaySnapshots[i].data(), r.size);
        }
    }
}

void OverlayStart(uint32_t id) {
    unsigned n = 0;
    for (unsigned i = 0; i < khvr_overlay_range_count; ++i) {
        const auto &r = khvr_overlay_ranges[i];
        if (r.id != id) continue;
        if (r.zero) std::memset(GuestMem() + r.addr, 0, r.size);
        else WriteBlock(r.addr, gOverlaySnapshots[i].data(), r.size);
        ++n;
    }
    Log("overlay: start ov%03u (%u data ranges reset)", id, n);
}

}  // namespace khvr::hle

extern "C" {
void w2c_env_khvr_host_read_rom(struct w2c_env *, u32 offset, u32 dst, u32 len) {
    using namespace khvr::hle;
    if (static_cast<uint64_t>(dst) + len > GuestMemSize()) {
        Log("rom: read 0x%x bytes to bad address 0x%08x", len, dst);
        return;
    }
    WatchHostWrite(dst, len, "ROM read");
    RomRead(offset, GuestMem() + dst, len);
}

void w2c_env_khvr_host_start_overlay(struct w2c_env *, u32 id) { khvr::hle::OverlayStart(id); }
}

// DS Protect (ov028) asks whether it runs from a genuine card: answer as the genuine card would.
// (The overlay itself is not built, see guest/exclude.txt; these are its entry points.)
extern "C" {
u32 w2c_env_Ov028_DSProt_DetectEmulator(struct w2c_env *, u32) { return 0; }
u32 w2c_env_Ov028_DSProt_DetectFlashcart(struct w2c_env *, u32) { return 0; }
u32 w2c_env_Ov028_DSProt_DetectDummy(struct w2c_env *, u32) { return 0; }
u32 w2c_env_Ov028_DSProt_DetectNotEmulator(struct w2c_env *, u32) { return 1; }
u32 w2c_env_Ov028_DSProt_DetectNotFlashcart(struct w2c_env *, u32) { return 1; }
u32 w2c_env_Ov028_DSProt_DetectNotDummy(struct w2c_env *, u32) { return 1; }
}
