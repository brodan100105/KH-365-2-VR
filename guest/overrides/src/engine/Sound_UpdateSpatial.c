/* Port copy of the decomp's Sound_UpdateSpatial (src/engine/) with a boot-log line now and then:
 * a positioned sound effect's distance to the listener, the listener's near and far distances
 * and full volume (+0xb46ec, +0xb46f0, +0xb46f4), and the volume it gets. Battle sounds (the
 * keyblade, voices) go through here and are silent in the port; menu sounds do not. */

#include "khvr_host.h"

extern unsigned char *gSoundMgr;
extern void VEC_Subtract(int *a, int *b, int *out);
extern int VEC_Mag(int *v);
extern int VEC_DotProduct(int *a, int *b);
extern int FX_Div(int num, int den);
extern void NNS_SndPlayerSetTrackPan(int *ch, int mask, int val);
extern void NNS_SndPlayerSetTrackVolume(int *ch, int mask, int val);

void Sound_UpdateSpatial(int param_1)
{
    static unsigned int calls;
    int base = (int)gSoundMgr;
    int delta[3];
    int vol;
    int dist = -1;

    if ((*(unsigned short *)(param_1 + 0x14) & 4) != 0) {
        vol = 0;
    } else {
        VEC_Subtract((int *)(param_1 + 8), (int *)(base + 0xb44cc), delta);
        dist = VEC_Mag(delta);
        if ((*(unsigned short *)(param_1 + 0x14) & 2) == 0) {
            int near = *(int *)(base + 0xb46ec);
            if (dist <= near) {
                vol = *(short *)(base + 0xb46f4);
            } else if (dist < *(int *)(base + 0xb46f0)) {
                int t = FX_Div(dist - near, *(int *)(base + 0xb46f0) - near);
                vol = (0x1000 - t) * (int)*(short *)(base + 0xb46f4) >> 0xc;
            } else {
                vol = 0;
            }
        } else {
            vol = 0x7f;
        }
        {
            int dot = VEC_DotProduct(delta, (int *)(base + 0xb44d8));
            int p = FX_Div(dot, dist);
            NNS_SndPlayerSetTrackPan((int *)(param_1 + 0x1c), 0xffff, (p << 7) >> 0xc);
        }
    }
    if (calls++ % 120 == 0) {
        khvr_host_trace("sound: positioned (flags, distance)", *(unsigned short *)(param_1 + 0x14), dist);
        khvr_host_trace("sound: listener near, far", *(int *)(base + 0xb46ec), *(int *)(base + 0xb46f0));
        khvr_host_trace("sound: full volume, volume", *(short *)(base + 0xb46f4), vol);
    }
    NNS_SndPlayerSetTrackVolume((int *)(param_1 + 0x1c), 0xffff, vol);
}
