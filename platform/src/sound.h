// The ARM7's sound driver, as far as the ARM9 can tell (sound.cpp): the NitroSDK's command list
// (SNDCommand) is run against a model of the DS's 16 sound channels, alarms are sent back the way
// the real driver sends them, and the channels are mixed into 48 kHz stereo once per frame, in
// step with the game's emulated time. Sequences (SSEQ) are a later step.
#pragma once

#include <cstddef>
#include <cstdint>

namespace khvr::hle {

void SoundReset();
// One PXI sound word from the ARM9: a list of SNDCommand {next, id, arg[4]} at `list`.
void SoundRunCommands(uint32_t list);
// Once per frame, between the guest's frames: alarms that came due, then one frame of samples.
void SoundFrame();
// The audio thread: up to `frames` stereo frames (interleaved L, R) of what has been mixed.
size_t SoundRead(int16_t *stereo, size_t frames);
// Movie sound (movie.cpp): mono or stereo 16-bit samples at `rate` Hz, mixed in as they come.
void SoundQueueMovie(const int16_t *samples, size_t frames, int channels, int rate);
void SoundStopMovie();

constexpr int kSoundOutputRate = 48000;

}  // namespace khvr::hle
