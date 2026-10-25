# Iteration 3 plan: movies and sound

Ready to start on "start iteration 3". Iteration 2 should finish first: the game has to get past
"Day 1" into the field (the action-script stall being diagnosed now), so there is gameplay to hear.

The goal: the opening and every cutscene movie play on the giant screen with their sound, and
the game has music and sound effects, with volume settings. Everything comes from your ROM on the
headset at runtime, as the game code does now; nothing copyrighted goes in the repository.

## What we found in the decomp

Most of the game's own movie player is decompiled, which is better than porting a separate
decoder (FFmpeg's MobiClip decoder was the fallback plan):

| Piece | State in the decomp | Work |
|---|---|---|
| Player, timing, presenter, scene hooks (`ov024_mobiclip/*.c`) | C, already in the port | none |
| Container, stream, frame stepping, ADPCM audio (`ov024_mobiclip/*.cpp`, 7 files) | C++ | compile C++ in the guest build (it only takes `.c` today, which is why `Ov024_MobiClip_MakeStream` was missing) |
| Frame decoder core | ARM code the game copies to ITCM and calls by pointer; **portable C++ version in `libs/mobiclip/video/portable/`** (1,700 lines) | compile it, point the decoder call at it |
| YCoCg-to-pixels blit (`Ov024_MobiClip_BlitRows`) | hand-written ARM, documented in its stub | write it in C (about 60 lines) |
| Audio transform decoder (coding 1) | hand-written ARM | not needed: KH Days' streams use IMA ADPCM (coding 3), already C++ |

Sound is the bigger half: the DS plays music and effects on its second processor (ARM7), whose
sound driver is not part of the decomp (it is ARM9 only). The game's NitroSystem sound library
(`libs/nns/snd`, 146 functions) runs on the ARM9 and sends the ARM7 command lists through the
shared work area (`SND_COMMAND_START_SEQ`, `SETUP_CHANNEL_PCM`, `CHANNEL_VOLUME`, `SETUP_ALARM`
and so on). The port acknowledges those today without playing anything. Iteration 3 replaces
that with a real ARM7 sound driver: a sequence player, a 16-channel mixer, and audio output.

## Part A: movies (picture first, then their sound through part B)

**Status (started):** the picture now goes a simpler way than steps 1-4 below. The platform
decodes the stream itself (`platform/src/movie.cpp`) with the decomp's portable decoder and its
two coefficient tables compiled natively, and the guest's existing player
(`guest/runtime/movie.c`, which keeps the game's timing) asks it for the frame that is due each
vertical blank; the picture replaces the top screen while the movie plays. The game's own C++
player, its ITCM decoder copy and its VRAM blit are not needed for the picture. The log reports
`movie: picture WxH, N frames` when a stream opens and how many frames decoded when it closes.
Movie sound still comes with part B (the stream's IMA-ADPCM audio follows the video bits in each
packet).

1. **C++ in the guest build.** `build_guest.py` compiles `.cpp` with clang++ for wasm32
   (`-fno-exceptions -fno-rtti`, the module's own operator new/delete and vtable data symbols the
   files already reference). Check: the 9 `.cpp` files compile and link; `MakeStream` resolves.
2. **Frame decoder hook.** Where the player copies its decoder to ITCM and stores the entry at
   +0x38 of the stream (`Ov024_MobiClip_ReserveTcmArenas`), the port stores a function that calls
   the portable `MobiClip_DecodeFrameCore` instead. Check: a unit test decodes a synthetic frame;
   on the headset the log reports frames decoded and their quantizer.
3. **`Ov024_MobiClip_BlitRows` in C**, from the stub's description (YCoCg ramp, checkerboard
   dither, 15-bit pixels into VRAM). Check: desktop test against a hand-made 16x2 block.
4. **Retire the picture-less movie** (`guest/runtime/movie.c`) once frames decode; keep it behind
   a setting (**Game > Movies: play / skip**) as a fallback.
5. **Giant screen during movies**: the movie lands in the DS's BG/sprite layers, which the 2D
   renderer already draws, subtitles included. Option: draw movie frames at a higher-quality
   scale (Sharp filter already helps).
6. **Movie sound** comes for free once part B plays the game's PCM channels (the player feeds its
   ADPCM output to a DS channel).

**You test:** the opening movie plays on the giant screen (screenshot); START still skips it; the
log lists frames decoded per second against the movie's rate.

## Part B: sound

1. **Audio output on the headset.** AAudio (Android's low-latency audio) through a small mixer
   thread: 48 kHz stereo, resampled from the DS's 32,768 Hz. Check: a test tone setting.
2. **The ARM7 sound driver's commands** (`hle_arm7.cpp` OnSound): decode every command list the
   ARM9 sends (today only `SHARED_WORK` is read) and keep the shared work area's channel and
   player status words the way the real driver does, since the ARM9 library polls them.
3. **The 16-channel mixer**: PCM8, PCM16 and IMA-ADPCM samples read from guest memory, PSG
   square waves and noise (channels 8-15), per-channel volume (with the DS's divider), pan and
   timer-based pitch, loops; the master volume. Check: unit tests with synthetic waves (exact
   sample output), no ROM needed.
4. **The sequence player**: the DS sequence format (SSEQ bytecode: notes, waits, tempo, program
   changes, pitch bend, volume/expression, modulation, loops/calls, variables), instruments from
   banks (SBNK: PCM, PSG, noise, drum sets and key splits) and wave archives (SWAR), ADSR
   envelopes, priorities and channel allocation, up to 16 players. The ARM9 library loads all of
   these into RAM from `sound_data.sdat` and passes their addresses in the commands. Check: unit
   tests with small hand-built sequences.
5. **Alarms and timers** (`SETUP_ALARM`, `START_TIMER`): stream playback and sound-effect timing
   depend on them.
6. **Streams**: `NNSi_SndArcStrm_MakeWaveData` (currently an unimplemented import; it fills the
   stream ring for streamed music). Check: the log stops reporting it missing.
7. **Settings**: Sound tab with master, music, effects and movie volume; mute when the headset
   is off.

**You test:** title screen music, menu sounds, then field music and effects; the log reports
players and channels in use. A recording through the Quest (or a short video) is the check.

## Order

1. A1-A4: movies with picture (no sound yet). One or two builds.
2. B1-B3: audio output and the channel mixer: movie sound and simple effects start working.
3. B4-B6: the sequence player and streams: music.
4. B7 and polish.

Each step ends in a build you install over the last one, with the log saying what it did.

## Risks

- The C++ files may need the same kind of fix-ups the C build needed (signature adapters,
  aliases); the build reports them.
- The sequence player is the biggest single piece and has to match the real driver's timing
  (it ticks at about 192 Hz) closely enough that music does not drift.
- Audio thread versus guest timing: the mixer runs on its own clock; the platform's frame loop
  already runs at the DS's rate, so the two are kept in step through the shared work counters.

## After iteration 3

Iteration 4 is the 3D world in VR (geometry capture, game camera hooks, panoramic and first
person in Twilight Town), then motion combat against the real enemies.
