// The sequence player of the ARM7 sound driver stand-in: the NitroSDK's SSEQ interpreter with
// its banks (SBNK) and wave archives (SWAR), voices with ADSR envelopes, pitch bend, LFO, sweep
// and portamento, and channel allocation by priority, all driven from the sound frame (about 192
// a second) as on the DS. The ARM9's NitroSystem loads the data into main RAM and passes its
// addresses; everything here reads it from there.
//
// Units are the SDK's: volumes in tenths of a decibel (0 is full, -723 silent), envelope levels
// in 1/128 of that, pitch in 1/64 of a semitone, timers as divisors of the channel clock.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "guest.h"
#include "hle.h"
#include "sound_internal.h"

extern "C" unsigned short khvr_snd_calc_channel_volume(int dB);

namespace khvr::hle {
namespace {

constexpr int kPlayers = 16, kTracks = 32, kTracksPerPlayer = 16;
constexpr int kDbMin = -723;
constexpr int kEnvelopeMin = kDbMin * 128;        // -92544: silent
constexpr uint32_t kPsgBaseTimer = 8006;          // a PSG or noise voice's timer at its original key
constexpr int kAllocNone = -1;

// Instrument types in a bank (SNDInstOffset's low byte).
enum InstType { kInstNull = 0, kInstPcm = 1, kInstPsg = 2, kInstNoise = 3, kInstDirectPcm = 4,
                kInstNullAlt = 5, kInstDrumSet = 16, kInstKeySplit = 17 };

struct Instrument {
    int type = kInstNull;
    uint32_t wave = 0;   // the SWAV (SNDWaveData) for PCM types
    int duty = 0;        // PSG
    int originalKey = 60;
    int attack = 127, decay = 127, sustain = 127, release = 127, pan = 64;
};

struct Track {
    bool active = false, noteWait = true, mute = false, tie = false, porta = false, cmp = true;
    // Allocated when the sequence was prepared (its 0xfe command) but not yet given its data by
    // the 0x93 that opens it: the game's track settings apply to it, but it does not play yet.
    bool opened = true;
    int player = -1;
    uint32_t base = 0, pc = 0;
    int wait = 0;
    int prg = 0;
    int volume = 127, volume2 = 127, pan = 0, panRange = 127, extPan = 0, extFader = 0, extPitch = 0;
    int bend = 0, bendRange = 2, transpose = 0, prio = 64;
    int attack = 0xff, decay = 0xff, sustain = 0xff, release = 0xff;
    int portaKey = 60, portaTime = 0, sweepPitch = 0;
    int modDepth = 0, modSpeed = 16, modType = 0, modRange = 1, modDelay = 0;
    uint16_t chMask = 0xffff;
    // Call and loop stack: return address and remaining count (loops), three deep.
    struct Frame { uint32_t pc; int count; bool loop; };
    Frame stack[3];
    int depth = 0;
};

struct Player {
    bool active = false, prepared = false, paused = false;
    int prio = 64, volume = 127, extFader = 0;
    int tempo = 120, tempoRatio = 256, tempoCounter = 0;
    uint32_t bank = 0;
    int track[kTracksPerPlayer];
    uint32_t ticks = 0;
};

enum EnvState { kAttack, kDecay, kSustain, kRelease };

struct Voice {
    bool on = false;
    int track = -1, player = -1;
    int key = 60, velocity = 127, length = -1;  // length in ticks, -1 until released
    int updates = 0;  // sound frames it has been updated in (the boot log's sound statistics)
    bool logged = false;  // one of the notes the boot log follows to their end
    int prio = 0;
    Instrument inst;
    uint32_t baseTimer = 0;
    // Envelope.
    EnvState env = kAttack;
    int level = kEnvelopeMin, attackRate = 0, decayRate = 0, sustainLevel = 0, releaseRate = 0;
    // Sweep (portamento or the track's sweep pitch): from sweepStart to 0 over sweepFrames.
    int sweepStart = 0, sweepFrames = 0, sweepCounter = 0;
    // LFO.
    int lfoDelay = 0, lfoCounter = 0;
};

std::array<Player, kPlayers> gPlayers;
std::array<Track, kTracks> gTrackPool;
std::array<Voice, 16> gVoice;  // one per channel
int16_t gLocalVars[kPlayers][16];
int16_t gGlobalVars[16];

// SNDi_DecibelSquareTable: 400 log10(v / 127), the curve the sequence volumes use.
int DecibelSquare(int v) {
    if (v <= 0) return kDbMin;
    if (v >= 127) return 0;
    return std::max(kDbMin, static_cast<int>(std::lround(400.0 * std::log10(v / 127.0))));
}

int CalcAttack(int a) {
    static const uint8_t kTable[19] = {0, 1, 5, 14, 26, 38, 51, 63, 73, 84, 92, 100, 109, 116, 123, 127, 132, 137, 143};
    a = std::clamp(a, 0, 127);
    return a < 109 ? 255 - a : kTable[127 - a];
}

int CalcRelease(int r) {
    r = std::clamp(r, 0, 127);
    if (r == 127) return 0xffff;
    if (r == 126) return 0x3c00;
    if (r < 50) return r * 2 + 1;
    return 0x1e00 / (126 - r);
}

// SND_SinIdx over a 128-step period: -127..127.
int SinIdx(int i) { return static_cast<int>(std::lround(127.0 * std::sin((i & 127) * 2.0 * 3.14159265358979 / 128.0))); }

int16_t &Var(int player, int n) {
    uint32_t shared = SoundSharedWork();
    if (n < 16) {
        if (shared) return *reinterpret_cast<int16_t *>(GuestMem() + shared + 0x20 + player * 0x24 + n * 2);
        return gLocalVars[player][n];
    }
    if (shared) return *reinterpret_cast<int16_t *>(GuestMem() + shared + 0x260 + (n - 16) * 2);
    return gGlobalVars[(n - 16) & 15];
}

// ---------------------------------------------------------------------------------- instruments

uint32_t WaveAddress(uint32_t arc, uint32_t index) {
    if (!arc || index >= Read32(arc + 0x38)) return 0;
    uint32_t off = Read32(arc + 0x3c + index * 4);
    if (!off) return 0;
    return off < 0x02000000 ? arc + off : off;
}

// The instrument data (type plus SNDInstParam) at `at`; `typed` for drum-set/key-split entries,
// which carry their type in front.
Instrument ReadInstrument(uint32_t bank, int type, uint32_t param) {
    Instrument in;
    in.type = type;
    uint16_t w0 = Read16(param), w1 = Read16(param + 2);
    in.originalKey = Read8(param + 4);
    in.attack = Read8(param + 5);
    in.decay = Read8(param + 6);
    in.sustain = Read8(param + 7);
    in.release = Read8(param + 8);
    in.pan = Read8(param + 9);
    switch (type) {
    case kInstPcm:
        if (w1 < 4) in.wave = WaveAddress(Read32(bank + 0x18 + w1 * 8), w0);
        if (!in.wave) in.type = kInstNull;
        break;
    case kInstDirectPcm:
        in.wave = w0 | static_cast<uint32_t>(w1) << 16;
        break;
    case kInstPsg:
        in.duty = w0 & 7;
        break;
    case kInstNoise:
        break;
    default:
        in.type = kInstNull;
        break;
    }
    return in;
}

Instrument FindInstrument(uint32_t bank, int prg, int key) {
    Instrument none;
    if (!bank || prg < 0 || static_cast<uint32_t>(prg) >= Read32(bank + 0x38)) return none;
    uint32_t entry = Read32(bank + 0x3c + prg * 4);
    int type = entry & 0xff;
    uint32_t at = bank + (entry >> 8);
    if (type == kInstDrumSet) {
        int lo = Read8(at), hi = Read8(at + 1);
        if (key < lo || key > hi) return none;
        uint32_t e = at + 2 + (key - lo) * 12;
        return ReadInstrument(bank, Read8(e), e + 2);
    }
    if (type == kInstKeySplit) {
        int i = 0;
        for (; i < 8; ++i) {
            int k = Read8(at + i);
            if (k == 0) return none;
            if (key <= k) break;
        }
        if (i == 8) return none;
        uint32_t e = at + 8 + i * 12;
        return ReadInstrument(bank, Read8(e), e + 2);
    }
    return ReadInstrument(bank, type, at);
}

// ---------------------------------------------------------------------------------- voices

void StopVoice(int ch) {
    Voice &v = gVoice[ch];
    if (v.on && v.logged && v.env != kRelease)
        Log("sound: note on channel %d cut after %d sound frames", ch, v.updates);
    v.on = false;
    SoundChannel &c = SoundChannels()[ch];
    if (c.owner == ch) {
        c.active = false;
        c.owner = -1;
    }
}

SeqNoteStats gStats;

void ReleaseVoice(Voice &v) {
    if (v.on && v.env != kRelease) v.env = kRelease;
}

// A channel for an instrument: the free ones of its kind the track may use, else the lowest
// priority voice at or below `prio`.
int AllocChannel(int type, uint16_t mask, int prio) {
    uint32_t kinds = type == kInstPsg ? 0x3f00u : type == kInstNoise ? 0xc000u : 0xffffu;
    kinds &= mask;
    auto &chans = SoundChannels();
    int best = kAllocNone, bestPrio = 1 << 30;
    // Prefer the order the SDK uses for PCM: from the top down, leaving 0-7 to streams last.
    static const int kOrder[16] = {4, 5, 6, 7, 2, 0, 3, 1, 8, 9, 10, 11, 14, 12, 15, 13};
    for (int n = 0; n < 16; ++n) {
        int i = kOrder[n];
        if (!(kinds & (1u << i)) || chans[i].locked) continue;
        const SoundChannel &c = chans[i];
        if (c.active && c.owner < 0) continue;  // the ARM9's own (a stream or sound effect)
        if (!gVoice[i].on) return i;
        int p = gVoice[i].env == kRelease ? -1 : gVoice[i].prio;
        if (p < bestPrio) {
            bestPrio = p;
            best = i;
        }
    }
    return best != kAllocNone && bestPrio <= prio ? best : kAllocNone;
}

void NoteOn(int ti, int key, int velocity, int length) {
    Track &t = gTrackPool[ti];
    Player &p = gPlayers[t.player];
    key = std::clamp(key, 0, 127);
    if (t.mute) return;
    // Tie: the note slides on the same voice.
    if (t.tie) {
        for (int ch = 0; ch < 16; ++ch) {
            Voice &v = gVoice[ch];
            if (v.on && v.track == ti && v.env != kRelease) {
                v.sweepStart = (v.key - key) * 64;
                v.sweepFrames = std::max(1, t.portaTime * 2);
                v.sweepCounter = 0;
                v.key = key;
                v.velocity = velocity;
                v.length = length;
                return;
            }
        }
    }
    Instrument inst = FindInstrument(p.bank, t.prg, key);
    // Notes that cannot sound are logged (a limited number), to find sound effects that go missing.
    static int missingLogged = 0, busyLogged = 0;
    if (inst.type == kInstNull) {
        ++gStats.missing;
        if (missingLogged++ < 40)
            Log("sound: player %d note %d: no instrument (bank 0x%08x program %d)", t.player, key, p.bank, t.prg);
        return;
    }
    int prio = std::clamp(t.prio + p.prio - 64, 0, 255);
    int ch = AllocChannel(inst.type, t.chMask, prio);
    if (ch == kAllocNone) {
        ++gStats.busy;
        if (busyLogged++ < 40)
            Log("sound: player %d note %d: no free channel (type %d, allowed 0x%04x, priority %d)", t.player, key,
                inst.type, t.chMask, prio);
        return;
    }
    if (gVoice[ch].on) StopVoice(ch);
    ++gStats.played;
    {
        int dB = DecibelSquare(velocity) + DecibelSquare(p.volume) + p.extFader + DecibelSquare(t.volume) +
                 DecibelSquare(t.volume2) + t.extFader;
        gStats.quietest = std::min(gStats.quietest, dB);
        gStats.loudest = std::max(gStats.loudest, dB);
    }
    SoundChannel &c = SoundChannels()[ch];
    Voice &v = gVoice[ch];
    v = Voice();
    v.on = true;
    v.track = ti;
    v.player = t.player;
    v.key = key;
    v.velocity = velocity;
    v.length = t.tie ? -1 : length;
    v.prio = prio;
    v.inst = inst;
    v.attackRate = CalcAttack(t.attack != 0xff ? t.attack : inst.attack);
    v.decayRate = CalcRelease(t.decay != 0xff ? t.decay : inst.decay);
    v.sustainLevel = DecibelSquare(t.sustain != 0xff ? t.sustain : inst.sustain) << 7;
    v.releaseRate = CalcRelease(t.release != 0xff ? t.release : inst.release);
    v.level = kEnvelopeMin;
    v.env = kAttack;
    v.lfoDelay = t.modDelay;
    // Portamento from the porta key, or the track's sweep pitch, glides to the note.
    int sweep = t.sweepPitch;
    if (t.porta) sweep += (t.portaKey - key) * 64;
    if (sweep) {
        v.sweepStart = sweep;
        v.sweepFrames = t.porta && t.portaTime ? t.portaTime * 2 : std::max(1, length * 2);
    }
    t.portaKey = key;
    c.owner = ch;
    c.volume = 0;
    c.shift = 0;
    if (inst.type == kInstPcm || inst.type == kInstDirectPcm) {
        uint32_t w = inst.wave;
        c.format = Read8(w) & 3;
        ++gStats.format[c.format < 3 ? c.format : 3];
        c.repeat = Read8(w + 1) ? kLoop : kOneShot;
        v.baseTimer = Read16(w + 4);
        c.loopStart = Read16(w + 6);
        c.loopLength = Read32(w + 8);
        c.source = w + 12;
        c.timer = v.baseTimer;
    } else {
        ++gStats.format[3];
        c.format = kPsg;
        c.duty = inst.duty;
        v.baseTimer = kPsgBaseTimer;
        c.timer = v.baseTimer;
    }
    SoundStartChannel(c);
    // The first notes of each session in full, to find sound effects that play but cannot be
    // heard: what the note asks for, the wave, and the level it starts at.
    static int notesLogged = 0;
    if (notesLogged < 80) {
        ++notesLogged;
        v.logged = true;
        int dB = DecibelSquare(velocity) + DecibelSquare(p.volume) + p.extFader + DecibelSquare(t.volume) +
                 DecibelSquare(t.volume2) + t.extFader;
        Log("sound: note player %d track %d channel %d key %d velocity %d length %d | instrument %d wave 0x%08x "
            "format %d timer %u loop %s %u+%u | level %d cB, attack %d release %d, pan %d",
            t.player, ti, ch, key, velocity, length, inst.type, inst.wave, c.format, c.timer,
            c.repeat == kLoop ? "on" : "off", c.loopStart, c.loopLength, dB, v.attackRate, v.releaseRate, c.pan);
    }
}

// The voice's channel parameters for this frame, and its envelope, length, sweep and LFO.
void UpdateVoice(int ch) {
    Voice &v = gVoice[ch];
    SoundChannel &c = SoundChannels()[ch];
    if (!v.on) return;
    if (c.owner != ch || !c.active) {  // stolen by the ARM9, or a one-shot wave ended
        if (v.updates == 0) ++gStats.endedAtOnce;
        if (v.logged)
            Log("sound: note on channel %d ended after %d sound frames (%s)", ch, v.updates,
                c.owner != ch ? "channel taken" : "wave ended");
        v.on = false;
        if (c.owner == ch) c.owner = -1;
        return;
    }
    ++v.updates;
    // Envelope.
    switch (v.env) {
    case kAttack:
        v.level = -((-v.level * v.attackRate) >> 8);
        if (v.level >= -128) {
            v.level = 0;
            v.env = kDecay;
        }
        break;
    case kDecay:
        v.level -= v.decayRate;
        if (v.level <= v.sustainLevel) {
            v.level = v.sustainLevel;
            v.env = kSustain;
        }
        break;
    case kSustain: break;
    case kRelease:
        v.level -= v.releaseRate;
        if (v.level <= kEnvelopeMin) {
            if (v.logged) Log("sound: note on channel %d released and silent after %d sound frames", ch, v.updates);
            StopVoice(ch);
            return;
        }
        break;
    }
    const Track *t = v.track >= 0 ? &gTrackPool[v.track] : nullptr;
    const Player &p = gPlayers[v.player];
    // LFO.
    int lfo = 0;
    if (t && t->modDepth) {
        if (v.lfoDelay > 0) {
            --v.lfoDelay;
        } else {
            v.lfoCounter += t->modSpeed << 6;
            lfo = SinIdx(v.lfoCounter >> 8) * t->modDepth * t->modRange;
        }
    }
    // Volume.
    int dB = (v.level >> 7) + DecibelSquare(v.velocity) + DecibelSquare(p.volume) + p.extFader;
    if (t) dB += DecibelSquare(t->volume) + DecibelSquare(t->volume2) + t->extFader;
    if (t && t->modType == 1) dB += lfo >> 8;
    unsigned short vol = khvr_snd_calc_channel_volume(std::max(dB, kDbMin));
    c.volume = vol & 0x7f;
    c.shift = (vol >> 8) & 3;
    // Pitch.
    int pitch = (v.key - v.inst.originalKey) * 64;
    if (t) pitch += (t->bend * t->bendRange * 64) >> 7, pitch += t->extPitch;
    if (v.sweepFrames > 0 && v.sweepCounter < v.sweepFrames) {
        pitch += v.sweepStart * (v.sweepFrames - v.sweepCounter) / v.sweepFrames;
        ++v.sweepCounter;
    }
    if (t && t->modType == 0) pitch += lfo >> 8;
    double timer = v.baseTimer / std::pow(2.0, pitch / 768.0);
    c.timer = static_cast<uint32_t>(std::clamp(timer, 16.0, 65535.0));
    // Pan.
    int pan = v.inst.pan - 64;
    if (t) pan += (t->pan * t->panRange) / 127 + t->extPan;
    if (t && t->modType == 2) pan += lfo >> 8;
    c.pan = std::clamp(64 + pan, 0, 127);
}

// ---------------------------------------------------------------------------------- tracks

void FreeTrack(int ti) {
    Track &t = gTrackPool[ti];
    for (int ch = 0; ch < 16; ++ch)
        if (gVoice[ch].on && gVoice[ch].track == ti) {
            ReleaseVoice(gVoice[ch]);
            gVoice[ch].track = -1;
        }
    t = Track();
}

int NewTrack(int player, uint32_t base, uint32_t pc) {
    for (int i = 0; i < kTracks; ++i) {
        if (gTrackPool[i].player >= 0) continue;
        Track &t = gTrackPool[i];
        t = Track();
        t.active = true;
        t.player = player;
        t.base = base;
        t.pc = pc;
        return i;
    }
    return -1;
}

void StopPlayer(int pn) {
    Player &p = gPlayers[pn];
    for (int i = 0; i < kTracksPerPlayer; ++i)
        if (p.track[i] >= 0) {
            FreeTrack(p.track[i]);
            p.track[i] = -1;
        }
    p.active = p.prepared = p.paused = false;
}

uint32_t ReadVlv(Track &t) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t b = Read8(t.pc++);
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return v;
}

enum ArgType { kArgU8, kArgS16, kArgVlv, kArgRandom, kArgVariable };

int ReadArg(Track &t, int player, ArgType type) {
    switch (type) {
    case kArgU8: return Read8(t.pc++);
    case kArgS16: {
        int v = static_cast<int16_t>(Read16(t.pc));
        t.pc += 2;
        return v;
    }
    case kArgVlv: return static_cast<int>(ReadVlv(t));
    case kArgRandom: {
        int lo = static_cast<int16_t>(Read16(t.pc)), hi = static_cast<int16_t>(Read16(t.pc + 2));
        t.pc += 4;
        if (hi < lo) std::swap(lo, hi);
        return lo + static_cast<int>(SoundRandom() % static_cast<uint32_t>(hi - lo + 1));
    }
    case kArgVariable: return Var(player, Read8(t.pc++) & 31);
    }
    return 0;
}

uint32_t Read24(Track &t) {
    uint32_t v = Read8(t.pc) | Read8(t.pc + 1) << 8 | Read8(t.pc + 2) << 16;
    t.pc += 3;
    return v;
}

// Runs one track until it waits; false once it ends.
bool StepTrack(int ti) {
    Track &t = gTrackPool[ti];
    if (!t.active) return false;
    if (t.wait > 0 && --t.wait > 0) return true;
    for (int guard = 0; guard < 1024; ++guard) {
        if (t.wait > 0) return true;
        uint8_t cmd = Read8(t.pc++);
        bool run = true;
        ArgType special = kArgU8;
        bool hasSpecial = false;
        if (cmd == 0xa2) {
            cmd = Read8(t.pc++);
            run = t.cmp;
        }
        if (cmd == 0xa0) {
            cmd = Read8(t.pc++);
            special = kArgRandom;
            hasSpecial = true;
        } else if (cmd == 0xa1) {
            cmd = Read8(t.pc++);
            special = kArgVariable;
            hasSpecial = true;
        }
        if (cmd < 0x80) {
            int velocity = Read8(t.pc++);
            int length = ReadArg(t, t.player, hasSpecial ? special : kArgVlv);
            if (run) {
                NoteOn(ti, cmd + t.transpose, velocity, length);
                if (t.noteWait) t.wait = length;
            }
            continue;
        }
        switch (cmd & 0xf0) {
        case 0x80: {
            int arg = ReadArg(t, t.player, hasSpecial ? special : kArgVlv);
            if (!run) break;
            if (cmd == 0x80) t.wait = arg;
            else if (cmd == 0x81 && arg < 0x10000) t.prg = arg;
            break;
        }
        case 0x90: {
            if (cmd == 0x93) {
                int n = Read8(t.pc++);
                uint32_t off = Read24(t);
                if (!run) break;
                Player &p = gPlayers[t.player];
                if (n < kTracksPerPlayer && p.track[n] < 0) {
                    int nt = NewTrack(t.player, t.base, t.base + off);
                    if (nt >= 0) p.track[n] = nt;
                } else if (n < kTracksPerPlayer && !gTrackPool[p.track[n]].opened) {
                    Track &o = gTrackPool[p.track[n]];
                    o.base = t.base;
                    o.pc = t.base + off;
                    o.opened = true;
                }
            } else if (cmd == 0x94) {
                uint32_t off = Read24(t);
                if (run) t.pc = t.base + off;
            } else if (cmd == 0x95) {
                uint32_t off = Read24(t);
                if (run && t.depth < 3) {
                    t.stack[t.depth++] = {t.pc, 0, false};
                    t.pc = t.base + off;
                }
            }
            break;
        }
        case 0xb0: {
            int n = Read8(t.pc++) & 31;
            int arg = ReadArg(t, t.player, hasSpecial ? special : kArgS16);
            if (!run) break;
            int16_t &var = Var(t.player, n);
            switch (cmd) {
            case 0xb0: var = static_cast<int16_t>(arg); break;
            case 0xb1: var = static_cast<int16_t>(var + arg); break;
            case 0xb2: var = static_cast<int16_t>(var - arg); break;
            case 0xb3: var = static_cast<int16_t>(var * arg); break;
            case 0xb4: if (arg) var = static_cast<int16_t>(var / arg); break;
            case 0xb5: var = static_cast<int16_t>(arg >= 0 ? var << arg : var >> -arg); break;
            case 0xb6: {
                bool neg = arg < 0;
                int r = static_cast<int>(SoundRandom() % static_cast<uint32_t>(std::abs(arg) + 1));
                var = static_cast<int16_t>(neg ? -r : r);
                break;
            }
            case 0xb8: t.cmp = var == arg; break;
            case 0xb9: t.cmp = var >= arg; break;
            case 0xba: t.cmp = var > arg; break;
            case 0xbb: t.cmp = var <= arg; break;
            case 0xbc: t.cmp = var < arg; break;
            case 0xbd: t.cmp = var != arg; break;
            default: break;
            }
            break;
        }
        case 0xc0:
        case 0xd0: {
            int arg = ReadArg(t, t.player, hasSpecial ? special : kArgU8);
            if (!run) break;
            switch (cmd) {
            case 0xc0: t.pan = arg - 64; break;
            case 0xc1: t.volume = arg; break;
            case 0xc2: gPlayers[t.player].volume = arg; break;
            case 0xc3: t.transpose = static_cast<int8_t>(arg); break;
            case 0xc4: t.bend = static_cast<int8_t>(arg); break;
            case 0xc5: t.bendRange = arg; break;
            case 0xc6: t.prio = arg; break;
            case 0xc7: t.noteWait = arg != 0; break;
            case 0xc8:
                t.tie = arg != 0;
                for (int ch = 0; ch < 16; ++ch)
                    if (gVoice[ch].on && gVoice[ch].track == ti) ReleaseVoice(gVoice[ch]);
                break;
            case 0xc9: t.portaKey = arg + t.transpose; t.porta = true; break;
            case 0xca: t.modDepth = arg; break;
            case 0xcb: t.modSpeed = arg; break;
            case 0xcc: t.modType = arg; break;
            case 0xcd: t.modRange = arg; break;
            case 0xce: t.porta = arg != 0; break;
            case 0xcf: t.portaTime = arg; break;
            case 0xd0: t.attack = arg; break;
            case 0xd1: t.decay = arg; break;
            case 0xd2: t.sustain = arg; break;
            case 0xd3: t.release = arg; break;
            case 0xd4:  // loop start: a count (0 = for ever)
                if (t.depth < 3) t.stack[t.depth++] = {t.pc, arg, true};
                break;
            case 0xd5: t.volume2 = arg; break;
            default: break;  // 0xd6 print variable
            }
            break;
        }
        case 0xe0: {
            int arg = ReadArg(t, t.player, hasSpecial ? special : kArgS16);
            if (!run) break;
            if (cmd == 0xe0) t.modDelay = arg;
            else if (cmd == 0xe1) gPlayers[t.player].tempo = arg;
            else if (cmd == 0xe3) t.sweepPitch = arg;
            break;
        }
        case 0xf0: {
            if (cmd == 0xfe) {  // the tracks the sequence opens: nothing to do
                t.pc += 2;
                break;
            }
            if (!run) break;
            if (cmd == 0xfc) {  // loop end
                if (t.depth > 0 && t.stack[t.depth - 1].loop) {
                    Track::Frame &f = t.stack[t.depth - 1];
                    if (f.count == 0) {
                        t.pc = f.pc;
                    } else if (--f.count > 0) {
                        t.pc = f.pc;
                    } else {
                        --t.depth;
                    }
                }
            } else if (cmd == 0xfd) {  // return
                if (t.depth > 0 && !t.stack[t.depth - 1].loop) t.pc = t.stack[--t.depth].pc;
            } else if (cmd == 0xff) {  // end of the track
                t.active = false;
                return false;
            }
            break;
        }
        default:
            break;
        }
    }
    return true;
}

// One tick of a player: every track; the player ends with its last track.
void TickPlayer(int pn) {
    Player &p = gPlayers[pn];
    bool any = false;
    for (int i = 0; i < kTracksPerPlayer; ++i) {
        int ti = p.track[i];
        if (ti < 0 || !gTrackPool[ti].opened) continue;
        if (StepTrack(ti)) any = true;
        else {
            FreeTrack(ti);
            p.track[i] = -1;
        }
    }
    // Note lengths count down per tick.
    for (int ch = 0; ch < 16; ++ch) {
        Voice &v = gVoice[ch];
        if (!v.on || v.player != pn || v.length < 0) continue;
        if (v.length > 0) --v.length;
        if (v.length == 0) ReleaseVoice(v);
    }
    ++p.ticks;
    if (!any) StopPlayer(pn);
}

void StartPlayer(int pn, uint32_t base, uint32_t offset, uint32_t bank, bool start) {
    static int logged = 0;
    if (logged++ < 300)
        Log("sound: %s player %d: sequence 0x%08x + 0x%x, bank 0x%08x", start ? "start" : "prepare", pn, base,
            offset, bank);
    if (pn < 0 || pn >= kPlayers) return;
    StopPlayer(pn);
    Player &p = gPlayers[pn];
    p = Player();
    for (int &t : p.track) t = -1;
    p.bank = bank;
    int ti = NewTrack(pn, base, base + offset);
    if (ti < 0) return;
    p.track[0] = ti;
    // As the DS's SND_PrepareSeq: a sequence that starts by allocating tracks (0xfe and a mask of
    // them) gets them now, so the settings the game sends between preparing and starting it
    // (track volume, the channels a track may use, muting) reach every track, not only the first.
    uint32_t pc = base + offset;
    if (Read8(pc) == 0xfe) {
        uint32_t mask = Read8(pc + 1) | static_cast<uint32_t>(Read8(pc + 2)) << 8;
        for (int i = 1; i < kTracksPerPlayer; ++i) {
            if (!(mask & (1u << i))) continue;
            int nt = NewTrack(pn, base, 0);
            if (nt < 0) break;
            gTrackPool[nt].opened = false;
            p.track[i] = nt;
        }
    }
    p.prepared = true;
    p.active = start;
}

}  // namespace

SeqNoteStats SeqStats() {
    SeqNoteStats s = gStats;
    gStats.quietest = 0;
    gStats.loudest = -100000;
    return s;
}

void SeqReset() {
    for (int i = 0; i < kTracks; ++i) gTrackPool[i] = Track();
    for (Player &p : gPlayers) {
        p = Player();
        for (int &t : p.track) t = -1;
    }
    for (Voice &v : gVoice) v = Voice();
    std::fill(&gLocalVars[0][0], &gLocalVars[0][0] + kPlayers * 16, 0);
    std::fill(gGlobalVars, gGlobalVars + 16, 0);
}

bool SeqCommand(uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    switch (id) {
    case 0x00: StartPlayer(static_cast<int>(a0), a1, a2, a3, true); return true;   // START_SEQ
    case 0x01: if (a0 < kPlayers) StopPlayer(static_cast<int>(a0)); return true;   // STOP_SEQ
    case 0x02: StartPlayer(static_cast<int>(a0), a1, a2, a3, false); return true;  // PREPARE_SEQ
    case 0x03:  // START_PREPARED_SEQ
        if (a0 < kPlayers && gPlayers[a0].prepared) {
            gPlayers[a0].active = true;
            ++gStats.startedPrepared;
        }
        return true;
    case 0x04:  // PAUSE_SEQ
        if (a0 < kPlayers) {
            gPlayers[a0].paused = a1 != 0;
            if (a1)
                for (Voice &v : gVoice)
                    if (v.on && v.player == static_cast<int>(a0)) ReleaseVoice(v);
        }
        return true;
    case 0x05:  // SKIP_SEQ: run ticks without sound
        if (a0 < kPlayers && gPlayers[a0].active) {
            for (uint32_t i = 0; i < a1 && gPlayers[a0].active; ++i) TickPlayer(static_cast<int>(a0));
            for (Voice &v : gVoice)
                if (v.on && v.player == static_cast<int>(a0)) ReleaseVoice(v);
        }
        return true;
    case 0x06: {  // PLAYER_PARAM: player, offset in SNDPlayer, data, size
        static int paramsLogged = 0;
        if (paramsLogged++ < 120)
            Log("sound: player %u parameter +0x%x = %d (%u bytes)", a0, a1, static_cast<int16_t>(a2), a3);
        if (a0 >= kPlayers) return true;
        Player &p = gPlayers[a0];
        switch (a1) {
        case 4: p.prio = a2 & 0xff; break;
        case 5: p.volume = a2 & 0x7f; break;
        case 6: p.extFader = static_cast<int16_t>(a2); break;
        case 0x18: p.tempo = a2 & 0xffff; break;
        case 0x1a: p.tempoRatio = a2 & 0xffff; break;
        default: LogOnce(("snd-pp-" + std::to_string(a1)).c_str(), "sound: player parameter at +0x%x not handled", a1); break;
        }
        return true;
    }
    case 0x07: {  // TRACK_PARAM: player | size << 24, track mask, offset in SNDTrack, data
        uint32_t pn = a0 & 0xffffff;
        static int trackParamsLogged = 0;
        if (trackParamsLogged++ < 120)
            Log("sound: player %u tracks 0x%04x parameter +0x%x = %d", pn, a1, a2, static_cast<int16_t>(a3));
        if (pn >= kPlayers) return true;
        for (int i = 0; i < kTracksPerPlayer; ++i) {
            if (!(a1 & (1u << i)) || gPlayers[pn].track[i] < 0) continue;
            Track &t = gTrackPool[gPlayers[pn].track[i]];
            switch (a2) {
            case 4: t.volume = a3 & 0x7f; break;
            case 5: t.volume2 = a3 & 0x7f; break;
            case 6: t.bend = static_cast<int8_t>(a3); break;
            case 7: t.bendRange = a3 & 0xff; break;
            case 8: t.pan = static_cast<int8_t>(a3); break;
            case 9: t.extPan = static_cast<int8_t>(a3); break;
            case 0xa: t.extFader = static_cast<int16_t>(a3); break;
            case 0xc: t.extPitch = static_cast<int16_t>(a3); break;
            case 0x12: t.prio = a3 & 0xff; break;
            case 0x13: t.transpose = static_cast<int8_t>(a3); break;
            default: LogOnce(("snd-tp-" + std::to_string(a2)).c_str(), "sound: track parameter at +0x%x not handled", a2); break;
            }
        }
        return true;
    }
    case 0x08:  // MUTE_TRACK: player, track mask, flag
        if (a0 < kPlayers)
            for (int i = 0; i < kTracksPerPlayer; ++i) {
                int ti = gPlayers[a0].track[i];
                if (ti < 0 || !(a1 & (1u << i))) continue;
                gTrackPool[ti].mute = a2 != 0;
                if (a2)
                    for (Voice &v : gVoice)
                        if (v.on && v.track == ti) ReleaseVoice(v);
            }
        return true;
    case 0x09:  // ALLOCATABLE_CHANNEL: player, track mask, channel mask
        if (a0 < kPlayers)
            for (int i = 0; i < kTracksPerPlayer; ++i) {
                int ti = gPlayers[a0].track[i];
                if (ti >= 0 && (a1 & (1u << i))) gTrackPool[ti].chMask = static_cast<uint16_t>(a2);
            }
        return true;
    case 0x0a:  // PLAYER_LOCAL_VAR: player, variable, value
        if (a0 < kPlayers && a1 < 16) Var(static_cast<int>(a0), static_cast<int>(a1)) = static_cast<int16_t>(a2);
        return true;
    case 0x0b:  // PLAYER_GLOBAL_VAR: variable, value
        if (a0 < 16) Var(0, 16 + static_cast<int>(a0)) = static_cast<int16_t>(a1);
        return true;
    case 0x21:  // READ_DRIVER_INFO: nothing the game's code reads from it yet
        return true;
    default:
        return false;
    }
}

void SeqFrame() {
    for (int pn = 0; pn < kPlayers; ++pn) {
        Player &p = gPlayers[pn];
        if (!p.active || p.paused) continue;
        p.tempoCounter += p.tempo * p.tempoRatio >> 8;
        for (int n = 0; p.tempoCounter >= 240 && n < 16 && p.active; ++n) {
            p.tempoCounter -= 240;
            TickPlayer(pn);
        }
    }
    for (int ch = 0; ch < 16; ++ch) UpdateVoice(ch);
}

void SeqUpdateShared() {
    uint32_t shared = SoundSharedWork();
    if (!shared) return;
    uint32_t status = 0;
    for (int pn = 0; pn < kPlayers; ++pn) {
        if (gPlayers[pn].active) status |= 1u << pn;
        Write32(shared + 0x20 + pn * 0x24 + 0x20, gPlayers[pn].ticks);
    }
    Write32(shared + 4, status);
}

}  // namespace khvr::hle
