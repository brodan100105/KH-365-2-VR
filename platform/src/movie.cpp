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
#include "sound.h"

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
    // Sound: IMA-ADPCM (codec 3) after each packet's video, per channel.
    int audioCodec = 0, audioChannels = 0, audioRate = 0;
    bool n3 = false;
    int adpcmSample[2] = {0, 0}, adpcmIndex[2] = {0, 0};
    std::vector<int16_t> audio;  // scratch: one packet's samples, interleaved
    uint64_t audioFrames = 0;
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

// One IMA-ADPCM nibble (ov024's Ov024_MobiClip_DecodeAdpcm: predictor clamped to 16 bits).
int16_t AdpcmNibble(int &sample, int &index, int code) {
    static const int8_t kIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    static const int16_t kStep[89] = {
        7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
        31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
        130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
        544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
        2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
        9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    int step = kStep[index];
    index = std::clamp(index + kIndex[code & 7], 0, 88);
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    sample = std::clamp(code & 8 ? sample - diff : sample + diff, -0x8000, 0x7fff);
    return static_cast<int16_t>(sample);
}

// A packet's sound (ov024's Ov024_MobiClip_StepAudio, kind 3): after the video's bytes, for each
// block and each channel 0x80 bytes of ADPCM (256 samples); a keyframe's packet (bit 15 of its
// first bitstream word) starts each channel's first block with a 4-byte header (index, then the
// sample at +2), and N3 streams put 4 more bytes after the video then.
void DecodeAudio(Movie &m, uint32_t payload, uint32_t payloadSize, uint32_t blocks, uint32_t videoBytes) {
    if (m.audioCodec != 3 || m.audioChannels < 1 || m.audioChannels > 2 || blocks == 0) return;
    const uint8_t *p = m.data + payload, *end = p + payloadSize;
    bool header = (Le16(p) & 0x8000) != 0;
    p += videoBytes + (header && m.n3 ? 4 : 0);
    const int chs = m.audioChannels;
    m.audio.assign(static_cast<size_t>(blocks) * 256 * chs, 0);
    for (uint32_t b = 0; b < blocks; ++b) {
        for (int ch = 0; ch < chs; ++ch) {
            if (b == 0 && header) {
                if (p + 4 > end) return;
                m.adpcmIndex[ch] = std::min<int>(p[0], 88);
                m.adpcmSample[ch] = static_cast<int16_t>(Le16(p + 2));
                p += 4;
            }
            if (p + 0x80 > end) return;
            int16_t *out = m.audio.data() + static_cast<size_t>(b) * 256 * chs + ch;
            for (int i = 0; i < 0x80; ++i) {
                out[(2 * i) * chs] = AdpcmNibble(m.adpcmSample[ch], m.adpcmIndex[ch], p[i] & 0xf);
                out[(2 * i + 1) * chs] = AdpcmNibble(m.adpcmSample[ch], m.adpcmIndex[ch], p[i] >> 4);
            }
            p += 0x80;
        }
    }
    SoundQueueMovie(m.audio.data(), static_cast<size_t>(blocks) * 256, chs, m.audioRate);
    m.audioFrames += static_cast<uint64_t>(blocks) * 256;
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
    DecodeAudio(m, payload, payloadSize, packed & 0x3fff, (result.bitsConsumed + 15) / 16 * 2);
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
    m->audioCodec = Le16(data + 0x18);
    m->audioChannels = Le16(data + 0x1a);
    m->audioRate = static_cast<int>(Le32(data + 0x1c));
    m->n3 = data[4] == 'N' && data[5] == '3';
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
    Log("movie: picture %ux%u, %u frames, %zu keyframes, sound codec %d x%d at %d Hz (%s)", m->width, m->height,
        m->frames, m->keyframes.size(), m->audioCodec, m->audioChannels, m->audioRate, path.c_str());
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
        Log("movie: picture closed after %u frames decoded (%u failed), %llu sound samples", gMovie->total,
            gMovie->failures, static_cast<unsigned long long>(gMovie->audioFrames));
        SoundStopMovie();
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
