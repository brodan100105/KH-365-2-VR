// The ARM7 sound driver stand-in (see sound.h). What the ARM9's NitroSDK sound library sends is a
// list of commands (snd_command.h ids 0x00..0x21) that the real driver runs on the ARM7 against
// the sound hardware; here the channel-level ones drive a model of the 16 channels (SOUNDxCNT and
// friends), which streamed music (NitroSystem's strm player refilling a ring buffer on alarms)
// needs, and the sequence commands go to the sequence player (sound_seq.cpp). Like the real
// driver it works in sound frames of 0xaa8 OS ticks (about 192 a second): alarms, then the
// sequences, then that frame's share of samples.
#include "sound.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "guest.h"
#include "hle.h"
#include "sound_internal.h"

namespace khvr::hle {

void Arm7PushReply(uint32_t tag, uint32_t data);  // hle_arm7.cpp

namespace {

constexpr uint32_t kTagSound = 7;
constexpr double kFrameRate = 59.8261;

const int kAdpcmIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
const int kAdpcmStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

using Channel = SoundChannel;

struct Alarm {
    bool set = false, running = false;
    uint32_t id = 0;
    double at = 0.0, period = 0.0;  // in OS ticks of emulated time
    double first = 0.0;             // the first one's delay, counted from START_TIMER
};

std::mutex gMu;
std::array<Channel, 16> gCh;
uint32_t gRandom = 0x12345678;
std::array<Alarm, 8> gAlarms;
double gTicks = 0.0;          // emulated OS ticks since reset
double gSamplesDue = 0.0;     // output samples owed (fractional)
double gSoundFramesDue = 0.0; // sound frames owed
int gMasterVolume = 127;
uint32_t gShared = 0;          // SNDSharedWork

// The output ring: interleaved stereo.
std::mutex gRingMu;
bool gPrimed = false;       // the output has its cushion (SoundRead)
uint32_t gUnderruns = 0;    // times the output ran dry
std::vector<int16_t> gRing(2 * kSoundOutputRate / 2);  // half a second
size_t gRingRead = 0, gRingWrite = 0, gRingFill = 0;

// Movie sound, resampled to the output rate as it is mixed.
std::vector<int16_t> gMovie;   // interleaved stereo at gMovieRate
double gMoviePos = 0.0;
int gMovieRate = 32768;

uint32_t SamplesPerWord(int format) { return format == kPcm8 ? 4 : format == kPcm16 ? 2 : 8; }

// ADPCM: one step, the DS's way (clamped to +-0x7fff).
void AdpcmStep(Channel &c, int nibble) {
    int step = kAdpcmStep[c.adpcmIndex];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    c.adpcmValue = (nibble & 8) ? std::max(c.adpcmValue - diff, -0x7fff) : std::min(c.adpcmValue + diff, 0x7fff);
    c.adpcmIndex = std::clamp(c.adpcmIndex + kAdpcmIndex[nibble & 7], 0, 88);
}

void StartChannelImpl(Channel &c) {
    c.active = true;
    // The channel starts now, which lies between two output samples: gSamplesDue is how far the
    // mix is behind now (negative when ahead), so the next sample mixed is that much before
    // (or after) the channel's start. Keeping that offset ties the channel to the sound clock
    // exactly, as the DS's channels and alarms share one.
    const double rate = c.timer ? kChannelClock / c.timer : 0.0;
    c.pos = -gSamplesDue * rate / kSoundOutputRate;
    c.decodedUpTo = 0;
    c.loopSaved = false;
    c.lfsr = 0x7fff;
    if (c.format == kAdpcm) {
        uint32_t header = Read32(c.source);
        c.adpcmValue = static_cast<int16_t>(header & 0xffff);
        c.adpcmIndex = std::min<int>((header >> 16) & 0x7f, 88);
    }
}

// The sample at `index` (in samples from the data's start), stepping ADPCM forward as needed.
int32_t SampleAt(Channel &c, uint32_t index) {
    switch (c.format) {
    case kPcm8: return static_cast<int8_t>(Read8(c.source + index)) << 8;
    case kPcm16: return static_cast<int16_t>(Read16(c.source + 2 * index));
    case kAdpcm: {
        const uint32_t loopSample = c.loopStart ? (c.loopStart - 1) * 8 : 0;  // PNT counts the header
        while (c.decodedUpTo <= index) {
            if (!c.loopSaved && c.decodedUpTo == loopSample) {
                c.loopSaved = true;
                c.loopValue = c.adpcmValue;
                c.loopIndex = c.adpcmIndex;
            }
            uint8_t byte = Read8(c.source + 4 + c.decodedUpTo / 2);
            c.adpcmPrev = c.adpcmValue;
            AdpcmStep(c, (c.decodedUpTo & 1) ? byte >> 4 : byte & 0xf);
            ++c.decodedUpTo;
        }
        return c.adpcmValue;
    }
    default: return 0;
    }
}

// One output sample of a channel at the output rate, advancing it.
int32_t NextSample(Channel &c, int index) {
    const double rate = c.timer ? kChannelClock / c.timer : 0.0;
    const double step = rate / kSoundOutputRate;
    if (c.format == kPsg) {
        int32_t s;
        if (index >= 14) {  // noise
            double before = c.pos;
            c.pos += step;
            for (int n = static_cast<int>(c.pos) - static_cast<int>(before); n > 0; --n) {
                bool carry = c.lfsr & 1;
                c.lfsr >>= 1;
                if (carry) c.lfsr ^= 0x6000;
            }
            s = (c.lfsr & 1) ? -0x7fff : 0x7fff;
        } else if (index >= 8) {  // square: duty is (d + 1) / 8 high
            int phase = static_cast<int>(c.pos) & 7;
            s = phase <= c.duty ? 0x7fff : -0x7fff;
            c.pos += step;
        } else {
            s = 0;
        }
        return s;
    }
    if (c.pos < 0) {  // not started yet within this output sample
        c.pos += step;
        return 0;
    }
    const uint32_t perWord = SamplesPerWord(c.format);
    const uint32_t header = c.format == kAdpcm ? 1 : 0;
    const uint32_t total = (c.loopStart + c.loopLength - header) * perWord;   // samples
    const uint32_t loopAt = (c.loopStart - (c.loopStart ? header : 0)) * perWord;
    uint32_t i = static_cast<uint32_t>(c.pos);
    if (i >= total) {
        if (c.repeat == kLoop && total > loopAt) {
            c.pos = loopAt + std::fmod(c.pos - loopAt, static_cast<double>(total - loopAt));
            i = static_cast<uint32_t>(c.pos);
            if (c.format == kAdpcm) {
                // The hardware restores the state it saved at the loop point.
                if (c.loopSaved) {
                    c.adpcmValue = c.loopValue;
                    c.adpcmIndex = c.loopIndex;
                }
                c.decodedUpTo = loopAt;
            }
        } else {
            c.active = false;
            return 0;
        }
    }
    // Linear interpolation between this sample and the next: picking the nearest one when the
    // channel's rate is converted to the output's adds a grainy edge to everything.
    const double frac = c.pos - i;
    int32_t s0, s1;
    if (c.format == kAdpcm) {
        // ADPCM decodes forwards only: decode the next sample and keep the one before it.
        if (i + 1 < total) {
            s1 = SampleAt(c, i + 1);
            s0 = c.adpcmPrev;
        } else {
            s0 = s1 = SampleAt(c, i);
        }
    } else {
        s0 = SampleAt(c, i);
        uint32_t j = i + 1;
        if (j >= total) j = (c.repeat == kLoop && total > loopAt) ? loopAt : i;
        s1 = SampleAt(c, j);
    }
    c.pos += step;
    return s0 + static_cast<int32_t>((s1 - s0) * frac);
}

void MixFrame(int frames) {
    std::vector<int16_t> out(static_cast<size_t>(frames) * 2);
    for (int f = 0; f < frames; ++f) {
        int64_t left = 0, right = 0;
        for (int i = 0; i < 16; ++i) {
            Channel &c = gCh[i];
            if (!c.active) continue;
            int64_t s = NextSample(c, i);
            static const int kShift[4] = {0, 1, 2, 4};
            s = (s * c.volume / 127) >> kShift[c.shift & 3];
            left += s * (127 - c.pan) / 127;
            right += s * c.pan / 127;
        }
        if (!gMovie.empty()) {
            size_t idx = static_cast<size_t>(gMoviePos) * 2;
            if (idx + 1 < gMovie.size()) {
                // The movie at the level of two full channels, interpolated like the channels.
                double frac = gMoviePos - std::floor(gMoviePos);
                bool next = idx + 3 < gMovie.size();
                double l = gMovie[idx] + (next ? (gMovie[idx + 2] - gMovie[idx]) * frac : 0.0);
                double r = gMovie[idx + 1] + (next ? (gMovie[idx + 3] - gMovie[idx + 1]) * frac : 0.0);
                left += static_cast<int64_t>(l * 2);
                right += static_cast<int64_t>(r * 2);
            }
            gMoviePos += static_cast<double>(gMovieRate) / kSoundOutputRate;
        }
        // 16 channels at full volume can reach 16x; the DS's mixer scales them down the same way.
        left = left * gMasterVolume / 127 / 4;
        right = right * gMasterVolume / 127 / 4;
        out[2 * f] = static_cast<int16_t>(std::clamp<int64_t>(left, -32768, 32767));
        out[2 * f + 1] = static_cast<int16_t>(std::clamp<int64_t>(right, -32768, 32767));
    }
    // Drop movie samples already played.
    size_t played = static_cast<size_t>(gMoviePos);
    if (played > 0 && !gMovie.empty()) {
        size_t n = std::min(played * 2, gMovie.size());
        gMovie.erase(gMovie.begin(), gMovie.begin() + static_cast<long>(n));
        gMoviePos -= static_cast<double>(n / 2);
    }
    std::lock_guard<std::mutex> lock(gRingMu);
    const size_t cap = gRing.size() / 2;
    for (int f = 0; f < frames; ++f) {
        if (gRingFill == cap) {  // the listener fell behind: drop the oldest
            gRingRead = (gRingRead + 1) % cap;
            --gRingFill;
        }
        gRing[2 * gRingWrite] = out[2 * f];
        gRing[2 * gRingWrite + 1] = out[2 * f + 1];
        gRingWrite = (gRingWrite + 1) % cap;
        ++gRingFill;
    }
}

void UpdateSharedWork() {
    if (!gShared) return;
    uint16_t status = 0;
    for (int i = 0; i < 16; ++i)
        if (gCh[i].active) status |= static_cast<uint16_t>(1u << i);
    Write16(gShared + 8, status);
    SeqUpdateShared();
}

void RunCommand(uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    switch (id) {
    case 0x0c:  // START_TIMER: channels, captures, alarms
        for (int i = 0; i < 16; ++i)
            if (a0 & (1u << i)) {
                StartChannelImpl(gCh[i]);
                gCh[i].owner = -1;  // the ARM9's own use: sequences keep off it while it plays
            }
        for (int i = 0; i < 8; ++i)
            if ((a2 & (1u << i)) && gAlarms[i].set) {
                gAlarms[i].running = true;
                gAlarms[i].at = gTicks + gAlarms[i].first;
                static int logged = 0;
                if (logged++ < 40)
                    Log("sound: alarm %d started (first after %.0f, then every %.0f OS ticks)", i, gAlarms[i].first,
                        gAlarms[i].period);
            }
        break;
    case 0x0d:  // STOP_TIMER
        for (int i = 0; i < 16; ++i)
            if (a0 & (1u << i)) gCh[i].active = false;
        for (int i = 0; i < 8; ++i)
            if (a2 & (1u << i)) gAlarms[i].running = false;
        break;
    case 0x0e: {  // SETUP_CHANNEL_PCM
        Channel &c = gCh[a0 & 0xf];
        c.active = false;
        c.owner = -1;
        c.timer = a0 >> 16;
        c.source = a1;
        c.loopLength = a2 & 0x3fffff;
        c.volume = (a2 >> 24) & 0x7f;
        c.shift = (a2 >> 22) & 3;
        c.loopStart = a3 & 0xffff;
        c.pan = (a3 >> 16) & 0x7f;
        c.format = (a3 >> 24) & 3;
        c.repeat = (a3 >> 26) & 3;
        break;
    }
    case 0x0f: {  // SETUP_CHANNEL_PSG: chNo | timer << 16, duty, volume | shift << 8, pan
        Channel &c = gCh[a0 & 0xf];
        c.active = false;
        c.owner = -1;
        c.format = kPsg;
        c.timer = a0 >> 16;
        c.duty = a1 & 7;
        c.volume = a2 & 0x7f;
        c.shift = (a2 >> 8) & 3;
        c.pan = a3 & 0x7f;
        break;
    }
    case 0x10: {  // SETUP_CHANNEL_NOISE
        Channel &c = gCh[a0 & 0xf];
        c.active = false;
        c.owner = -1;
        c.format = kPsg;
        c.timer = a0 >> 16;
        c.volume = a1 & 0x7f;
        c.shift = (a1 >> 8) & 3;
        c.pan = a2 & 0x7f;
        break;
    }
    case 0x12: {  // SETUP_ALARM: id, first tick, period, generation
        Alarm &al = gAlarms[a0 & 7];
        al.set = true;
        al.running = false;
        al.id = a3 & 0xff;
        al.first = a1;
        al.period = a2;
        break;
    }
    case 0x13:  // CHANNEL_TIMER: mask, timer
        for (int i = 0; i < 16; ++i)
            if (a0 & (1u << i)) gCh[i].timer = a1 & 0xffff;
        break;
    case 0x14:  // CHANNEL_VOLUME: mask, volume, shift
        for (int i = 0; i < 16; ++i)
            if (a0 & (1u << i)) {
                gCh[i].volume = a1 & 0x7f;
                gCh[i].shift = a2 & 3;
            }
        break;
    case 0x15:  // CHANNEL_PAN: mask, pan
        for (int i = 0; i < 16; ++i)
            if (a0 & (1u << i)) gCh[i].pan = a1 & 0x7f;
        break;
    case 0x17:  // MASTER_VOLUME
        gMasterVolume = a0 & 0x7f;
        break;
    case 0x1a:  // LOCK_CHANNEL
        for (int i = 0; i < 16; ++i)
            if (a0 & (1u << i)) gCh[i].locked = true;
        break;
    case 0x1b:  // UNLOCK_CHANNEL
        for (int i = 0; i < 16; ++i)
            if (a0 & (1u << i)) gCh[i].locked = false;
        break;
    case 0x1c:  // STOP_UNLOCKED_CHANNEL
        for (int i = 0; i < 16; ++i)
            if ((a0 & (1u << i)) && !gCh[i].locked) gCh[i].active = false;
        break;
    case 0x1d:  // SHARED_WORK
        gShared = a0;
        Log("sound: shared work at 0x%08x", gShared);
        break;
    case 0x16: case 0x18: case 0x19: case 0x1e: case 0x1f: case 0x20:
        break;  // surround, master pan, output selector, cache invalidation: nothing to do
    default:
        if (!SeqCommand(id, a0, a1, a2, a3))
            LogOnce(("snd-cmd-" + std::to_string(id)).c_str(), "sound: command 0x%02x not played", id);
        break;
    }
}

}  // namespace

std::array<SoundChannel, 16> &SoundChannels() { return gCh; }
void SoundStartChannel(SoundChannel &c) { StartChannelImpl(c); }
uint32_t SoundSharedWork() { return gShared; }
uint32_t SoundRandom() {
    gRandom = gRandom * 1664525u + 1013904223u;
    return gRandom >> 16;
}

void SoundReset() {
    std::lock_guard<std::mutex> lock(gMu);
    gCh = {};
    gAlarms = {};
    gTicks = 0.0;
    gSamplesDue = 0.0;
    gSoundFramesDue = 0.0;
    gMasterVolume = 127;
    gShared = 0;
    gMovie.clear();
    gMoviePos = 0.0;
    SeqReset();
    std::lock_guard<std::mutex> ring(gRingMu);
    gRingRead = gRingWrite = gRingFill = 0;
    gPrimed = false;
}

void SoundRunCommands(uint32_t list) {
    std::lock_guard<std::mutex> lock(gMu);
    for (int guard = 0; list && guard < 4096; ++guard) {
        RunCommand(Read32(list + 4), Read32(list + 8), Read32(list + 12), Read32(list + 16), Read32(list + 20));
        list = Read32(list);
    }
    if (gShared) Write32(gShared, Read32(gShared) + 1);  // finishCommandTag
    UpdateSharedWork();
}

void SoundFrame(const std::function<void()> &respond) {
    std::unique_lock<std::mutex> lock(gMu);
    // One video frame's worth of sound frames. Each runs the sequences, then mixes its samples up
    // to each alarm as it comes due and fires it there: a stream's alarm marks the moment its
    // channel has played a part of the buffer, which the game then refills. Fired early, the game
    // would overwrite sound not played yet; fired late, the channel would play stale sound.
    gSoundFramesDue += kSoundFrameRate / kFrameRate;
    const double frameTicks = kOsTickRate / kSoundFrameRate;
    auto mixTicks = [](double ticks) {
        gSamplesDue += ticks * kSoundOutputRate / kOsTickRate;
        int frames = static_cast<int>(gSamplesDue);
        gSamplesDue -= frames;
        if (frames > 0) MixFrame(frames);
    };
    for (; gSoundFramesDue >= 1.0; gSoundFramesDue -= 1.0) {
        const double end = gTicks + frameTicks;
        SeqFrame();
        for (int guard = 0; guard < 256; ++guard) {
            double next = end;
            for (const Alarm &al : gAlarms)
                if (al.running && al.at < next) next = al.at;
            if (next > gTicks) {
                mixTicks(next - gTicks);
                gTicks = next;
            }
            if (next >= end) break;
            // Also the output sample that straddles the alarm: it still belongs to what has played.
            if (gSamplesDue > 0) {
                MixFrame(1);
                gSamplesDue -= 1.0;
            }
            bool fired = false;
            for (Alarm &al : gAlarms) {
                if (!al.running) continue;
                for (int n = 0; al.at <= gTicks && n < 64; ++n) {
                    Arm7PushReply(kTagSound, (&al - gAlarms.data()) | (al.id << 8));
                    fired = true;
                    if (al.period <= 0) {
                        al.running = false;
                        break;
                    }
                    al.at += al.period;
                }
            }
            if (fired && respond) {
                // On the DS the ARM9 answers within a millisecond. The guest sends sound commands
                // meanwhile, so the lock is let go.
                lock.unlock();
                respond();
                lock.lock();
            }
        }
        gTicks = end;
    }
    UpdateSharedWork();
}

size_t SoundRead(int16_t *stereo, size_t frames) {
    std::lock_guard<std::mutex> lock(gRingMu);
    const size_t cap = gRing.size() / 2;
    // The game mixes a video frame's samples at once (~800), the output takes them a few hundred
    // at a time. Keep a cushion of three video frames so a late frame does not leave a gap (each
    // gap is a click): start, and start again after running dry, only once it is there.
    const size_t cushion = kSoundOutputRate * 3 / 60;
    if (!gPrimed) {
        if (gRingFill < cushion + frames) {
            for (size_t i = 0; i < frames; ++i) stereo[2 * i] = stereo[2 * i + 1] = 0;
            return 0;
        }
        gPrimed = true;
    }
    // Keep the latency low: well past the cushion (the game ran ahead), skip ahead to it.
    const size_t maxFill = cushion * 3;
    if (gRingFill > maxFill + frames) {
        size_t skip = gRingFill - cushion - frames;
        gRingRead = (gRingRead + skip) % cap;
        gRingFill -= skip;
    }
    size_t n = std::min(frames, gRingFill);
    if (n < frames) {
        gPrimed = false;
        ++gUnderruns;
    }
    for (size_t i = 0; i < n; ++i) {
        stereo[2 * i] = gRing[2 * gRingRead];
        stereo[2 * i + 1] = gRing[2 * gRingRead + 1];
        gRingRead = (gRingRead + 1) % cap;
    }
    gRingFill -= n;
    for (size_t i = n; i < frames; ++i) stereo[2 * i] = stereo[2 * i + 1] = 0;
    return n;
}

uint32_t SoundUnderruns() {
    std::lock_guard<std::mutex> lock(gRingMu);
    return gUnderruns;
}

void SoundQueueMovie(const int16_t *samples, size_t frames, int channels, int rate) {
    std::lock_guard<std::mutex> lock(gMu);
    gMovieRate = rate > 0 ? rate : 32768;
    for (size_t i = 0; i < frames; ++i) {
        int16_t l = samples[i * channels], r = channels > 1 ? samples[i * channels + 1] : l;
        gMovie.push_back(l);
        gMovie.push_back(r);
    }
    // Never hold more than two seconds (a stalled mixer must not grow this without bound).
    const size_t limit = static_cast<size_t>(gMovieRate) * 2 * 2;
    if (gMovie.size() > limit) gMovie.erase(gMovie.begin(), gMovie.end() - static_cast<long>(limit));
}

void SoundStopMovie() {
    std::lock_guard<std::mutex> lock(gMu);
    gMovie.clear();
    gMoviePos = 0.0;
}

}  // namespace khvr::hle
