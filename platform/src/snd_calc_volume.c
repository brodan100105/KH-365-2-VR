/* The NitroSDK's SND_CalcChannelVolume (decomp: libs/nitro/snd/calls), compiled for the host
 * sound driver (sound_seq.cpp): -72.3..0 dB (in tenths) to a 7-bit volume and data shift. */
#include "../../decomp/libs/nitro/snd/calls/SND_CalcChannelVolume.c"

unsigned short khvr_snd_calc_channel_volume(int dB) { return SND_CalcChannelVolume(dB); }
