// What the sound driver stand-in's two halves share (sound.cpp: the channels, mixer and
// channel-level commands; sound_seq.cpp: the sequence player).
#pragma once

#include <array>
#include <cstdint>

namespace khvr::hle {

constexpr double kArm7Clock = 33513982.0;           // bus clock
constexpr double kChannelClock = kArm7Clock / 2.0;  // channel timers count at half of it
constexpr double kOsTickRate = kArm7Clock / 64.0;   // OS_GetTick units (alarm times)
// The ARM7 driver's sound frame (SND_PROC_INTERVAL): 0xaa8 OS ticks, about 192 per second.
constexpr double kSoundFrameRate = kOsTickRate / 0xaa8;

enum SoundFormat { kPcm8 = 0, kPcm16 = 1, kAdpcm = 2, kPsg = 3 };
enum SoundRepeat { kManual = 0, kLoop = 1, kOneShot = 2 };

struct SoundChannel {
    bool active = false;
    int format = kPcm8, repeat = kOneShot, duty = 0;
    uint32_t source = 0;       // SOUNDxSAD
    uint32_t loopStart = 0;    // SOUNDxPNT, in words
    uint32_t loopLength = 0;   // SOUNDxLEN, in words
    uint32_t timer = 0;        // the timer divisor, as the SDK passes it (the register holds 0x10000 - it)
    int volume = 0, shift = 0, pan = 64;
    bool locked = false;       // LOCK_CHANNEL: kept from sequences
    int owner = -1;            // the sequence voice playing on it, or -1 (direct use)
    // Playback.
    double pos = 0.0;          // in samples from the start of the data (past an ADPCM header)
    uint32_t decodedUpTo = 0;  // ADPCM: samples decoded so far
    int adpcmValue = 0, adpcmIndex = 0;
    int adpcmPrev = 0;         // the sample before adpcmValue, for interpolation
    bool loopSaved = false;
    int loopValue = 0, loopIndex = 0;
    uint16_t lfsr = 0x7fff;    // noise
};

std::array<SoundChannel, 16> &SoundChannels();
void SoundStartChannel(SoundChannel &c);
uint32_t SoundSharedWork();
uint32_t SoundRandom();  // SND_CalcRandom

// The sequence player (sound_seq.cpp), under the driver's lock.
void SeqReset();
bool SeqCommand(uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);  // false: not a sequence command
void SeqFrame();         // one sound frame: players step, voices update their channels
// Notes since boot: played, without an instrument in the bank, without a free channel; and
// prepared sequences started (START_PREPARED_SEQ).
struct SeqNoteStats {
    uint32_t played = 0, missing = 0, busy = 0, startedPrepared = 0;
    // Notes played by wave format (PCM8, PCM16, ADPCM, PSG or noise), notes whose channel was gone
    // by their first update (a wave that ended at once, or taken by the ARM9), and the quietest
    // and loudest starting level (velocity, player and track volumes and faders, in centibels
    // where 0 is full and -723 silence) of the notes since the counts were last read.
    uint32_t format[4] = {};
    uint32_t endedAtOnce = 0;
    int quietest = 0, loudest = -100000;
};
SeqNoteStats SeqStats();
void SeqUpdateShared();  // player status and tick counters into the shared work

}  // namespace khvr::hle
