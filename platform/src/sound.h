// The ARM7's sound driver, as far as the ARM9 can tell (sound.cpp): the NitroSDK's command list
// (SNDCommand) is run against a model of the DS's 16 sound channels, alarms are sent back the way
// the real driver sends them, and the channels are mixed into 48 kHz stereo once per frame, in
// step with the game's emulated time. Sequences (SSEQ) are a later step.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace khvr::hle {

void SoundReset();
// One PXI sound word from the ARM9: a list of SNDCommand {next, id, arg[4]} at `list`.
void SoundRunCommands(uint32_t list);
// Once per frame, between the guest's frames: the ~3.2 sound frames of a video frame, each its
// alarms, then its sequences and samples. `respond` runs after a sound frame that fired alarms,
// before its samples, to let the guest handle them (the platform wakes it).
void SoundFrame(const std::function<void()> &respond = {});
// The audio thread: up to `frames` stereo frames (interleaved L, R) of what has been mixed.
size_t SoundRead(int16_t *stereo, size_t frames);
// Movie sound (movie.cpp): mono or stereo 16-bit samples at `rate` Hz, mixed in as they come.
void SoundQueueMovie(const int16_t *samples, size_t frames, int channels, int rate);
void SoundStopMovie();
// How often the output ran out of mixed samples (each time is a short gap).
uint32_t SoundUnderruns();

constexpr int kSoundOutputRate = 48000;

}  // namespace khvr::hle
