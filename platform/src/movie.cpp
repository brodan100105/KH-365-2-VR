// Movies with a picture: the game's MobiClip streams (.mods) decoded on the host with the
// decomp's portable decoder (decomp/libs/mobiclip/video/portable, checked frame for frame against
// FFmpeg there), driven by the guest's picture-less player (guest/runtime/movie.c), which keeps
// the game's timing and tells the platform which frame is due each vertical blank. The frame
// replaces the top screen while the movie plays.
#include "movie.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "guest.h"
#include "hle.h"
#include "mobiclip_reference.hpp"

extern "C" const void *khvr_mobiclip_vlc0(void);
extern "C" const void *khvr_mobiclip_vlc1(void);

namespace khvr::hle {
namespace {

using namespace khdays::mobiclip;

constexpr unsigned kStride = 256;     // the decoder's plane stride (MOBICLIP_PLANE_STRIDE)
constexpr unsigned kHistory = 6;      // the frame being decoded and the five before it
constexpr unsigned kMaxCatchUp = 8;   // frames decoded per call before skipping to a keyframe
constexpr unsigned kHeaderSize = 0x30;

uint32_t Le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint16_t Le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

struct Movie {
    const uint8_t *data = nullptr;  // the file, in the mapped ROM
    uint32_t size = 0;
    uint32_t width = 0, height = 0, frames = 0;
    uint32_t packetStart = 0, packetEnd = 0;
    std::vector<std::pair<uint32_t, uint32_t>> keyframes;  // (frame, packet offset)
    // Decoder state.
    std::vector<uint8_t> luma[kHistory], chroma[kHistory];
    unsigned newest = 0;     // index of the newest decoded frame's planes
    unsigned decoded = 0;    // frames held in the history
    uint32_t next = 0;       // the next frame to decode
    uint32_t nextOffset = 0; // its packet
    unsigned quantizer = 12;
    bool formatVariant = false;
    uint32_t failures = 0, total = 0;
    // The latest picture, RGBA (0xAABBGGRR), 256 x 192, letterboxed.
    std::vector<uint32_t> rgba;
    bool havePicture = false;
};

std::mutex gMu;
Movie *gMovie = nullptr;

void ResetDecoder(Movie &m, uint32_t frame, uint32_t offset) {
    m.next = frame;
    m.nextOffset = offset;
    m.decoded = 0;
    m.quantizer = 12;
    m.formatVariant = false;
}

bool DecodeNext(Movie &m) {
    if (m.next >= m.frames || m.nextOffset + 4 > m.packetEnd) return false;
    uint32_t packed = Le32(m.data + m.nextOffset);
    uint32_t payload = m.nextOffset + 4, payloadSize = packed >> 14;
    if (payload + payloadSize > m.packetEnd) return false;

    unsigned slot = (m.newest + 1) % kHistory;
    CoefficientTable tables[2];
    for (int t = 0; t < 2; ++t) {
        const uint8_t *table = static_cast<const uint8_t *>(t ? khvr_mobiclip_vlc1() : khvr_mobiclip_vlc0());
        tables[t].lookup = reinterpret_cast<const unsigned short *>(table);
        tables[t].residue = table + 4096 * 2;
    }
    DecoderReferenceFrame histories[kHistory - 1];
    unsigned historyCount = std::min<unsigned>(m.decoded, kHistory - 1);
    for (unsigned i = 0; i < historyCount; ++i) {
        unsigned h = (m.newest + kHistory - i) % kHistory;
        histories[i] = {m.luma[h].data(), m.chroma[h].data(), m.chroma[h].data() + kStride / 2, kStride, kStride};
    }
    DecoderFrameBuffer out{m.luma[slot].data(), m.chroma[slot].data(), m.chroma[slot].data() + kStride / 2, kStride,
                           kStride};
    MotionVector motion[kStride / 16 + 3];
    DecoderFrameResult result{};
    // The decoder may read a little past the frame: it is given the rest of the packet area.
    bool ok = decodeFrame(m.data + payload, m.packetEnd - payload, m.width, m.height, tables, histories,
                          historyCount, m.quantizer, m.formatVariant, out, motion, kStride / 16 + 3, result);
    ++m.total;
    m.next++;
    m.nextOffset = payload + payloadSize;
    if (!ok) {
        ++m.failures;
        return true;  // keep the previous picture, try the next frame
    }
    m.quantizer = result.header.quantizer;
    m.formatVariant = result.header.formatVariant;
    m.newest = slot;
    m.decoded = std::min<unsigned>(m.decoded + 1, kHistory);
    return true;
}

// The newest frame to RGBA: YCoCg as the game's own converter reads it (Co in the first half of
// each chroma row, Cg in the second), at full 8-bit precision instead of the DS's 5 bits.
void Convert(Movie &m) {
    if (!m.decoded) return;
    const uint8_t *y0 = m.luma[m.newest].data(), *c0 = m.chroma[m.newest].data();
    std::fill(m.rgba.begin(), m.rgba.end(), 0xff000000u);
    uint32_t w = std::min<uint32_t>(m.width, 256), h = std::min<uint32_t>(m.height, 192);
    uint32_t ox = (256 - w) / 2, oy = (192 - h) / 2;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *luma = y0 + y * kStride, *chroma = c0 + (y / 2) * kStride;
        uint32_t *dst = m.rgba.data() + (oy + y) * 256 + ox;
        for (uint32_t x = 0; x < w; ++x) {
            int s = luma[x], co = chroma[x / 2] - 128, cg = chroma[kStride / 2 + x / 2] - 128;
            int r = std::clamp(s + co - cg, 0, 255), g = std::clamp(s + cg, 0, 255),
                b = std::clamp(s - co - cg, 0, 255);
            dst[x] = 0xff000000u | static_cast<uint32_t>(b) << 16 | static_cast<uint32_t>(g) << 8 |
                     static_cast<uint32_t>(r);
        }
    }
    m.havePicture = true;
}

}  // namespace

bool MovieOpen(const std::string &path) {
    std::lock_guard<std::mutex> lock(gMu);
    delete gMovie;
    gMovie = nullptr;
    std::string p = path;
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    uint32_t size = 0;
    const uint8_t *data = RomFindFile(p, &size);
    if (!data || size < kHeaderSize || std::memcmp(data, "MODS", 4) != 0) {
        Log("movie: cannot open %s for its picture", path.c_str());
        return false;
    }
    auto *m = new Movie;
    m->data = data;
    m->size = size;
    m->frames = Le32(data + 8);
    m->width = Le32(data + 0x0c);
    m->height = Le32(data + 0x10);
    uint32_t audioInfo = Le32(data + 0x24), keyTable = Le32(data + 0x28), keyCount = Le32(data + 0x2c);
    uint32_t start = kHeaderSize;
    if (data[4] == 'N' && data[5] == '3') {  // N3: a parameter list ending with "HE"
        while (start + 4 <= size) {
            bool he = data[start] == 'H' && data[start + 1] == 'E';
            start += 4 + Le16(data + start + 2) * 4u;
            if (he) break;
        }
    }
    uint32_t end = size;
    if (audioInfo > start && audioInfo < end) end = audioInfo;
    if (keyTable > start && keyTable < end) end = keyTable;
    m->packetStart = start;
    m->packetEnd = end;
    if (keyTable && keyTable + keyCount * 8ull <= size)
        for (uint32_t i = 0; i < keyCount; ++i)
            m->keyframes.emplace_back(Le32(data + keyTable + 8 * i), Le32(data + keyTable + 8 * i + 4));
    if (m->width == 0 || m->width > kStride || m->height == 0 || m->height > 256) {
        Log("movie: %s is %ux%u, which the decoder does not take", path.c_str(), m->width, m->height);
        delete m;
        return false;
    }
    for (unsigned i = 0; i < kHistory; ++i) {
        m->luma[i].assign(kStride * m->height, 0);
        m->chroma[i].assign(kStride * ((m->height + 1) / 2), 128);
    }
    m->rgba.assign(256 * 192, 0xff000000u);
    ResetDecoder(*m, 0, start);
    gMovie = m;
    Log("movie: picture %ux%u, %u frames, %zu keyframes (%s)", m->width, m->height, m->frames, m->keyframes.size(),
        path.c_str());
    return true;
}

void MovieShowFrame(uint32_t frame) {
    std::lock_guard<std::mutex> lock(gMu);
    Movie *m = gMovie;
    if (!m) return;
    if (frame >= m->frames) frame = m->frames ? m->frames - 1 : 0;
    if (m->next > frame + 1 || frame >= m->next + kMaxCatchUp) {
        // Behind (or asked for an earlier frame): restart at the last keyframe at or before it.
        std::pair<uint32_t, uint32_t> best{0, m->packetStart};
        for (auto &k : m->keyframes)
            if (k.first <= frame && k.first >= best.first) best = k;
        if (best.first > m->next || m->next > frame + 1) ResetDecoder(*m, best.first, best.second);
    }
    bool any = false;
    for (unsigned i = 0; i < kMaxCatchUp && m->next <= frame; ++i) {
        if (!DecodeNext(*m)) break;
        any = true;
    }
    if (any) Convert(*m);
}

void MovieClose() {
    std::lock_guard<std::mutex> lock(gMu);
    if (gMovie) {
        Log("movie: picture closed after %u frames decoded (%u failed)", gMovie->total, gMovie->failures);
        delete gMovie;
        gMovie = nullptr;
    }
}

bool MovieCopyPicture(uint32_t *rgba256x192) {
    std::lock_guard<std::mutex> lock(gMu);
    if (!gMovie || !gMovie->havePicture) return false;
    std::memcpy(rgba256x192, gMovie->rgba.data(), 256 * 192 * 4);
    return true;
}

}  // namespace khvr::hle

// The guest's side (guest/runtime/movie.c).
extern "C" {
struct w2c_env;
void w2c_env_khvr_host_movie_open(struct w2c_env *, uint32_t path) {
    khvr::hle::MovieOpen(khvr::hle::ReadString(path));
}
void w2c_env_khvr_host_movie_frame(struct w2c_env *, uint32_t frame) { khvr::hle::MovieShowFrame(frame); }
void w2c_env_khvr_host_movie_close(struct w2c_env *) { khvr::hle::MovieClose(); }
}
