/* Dispatches the model's callbacks with the flag, or with the camera-distance LOD when not forced.
 *
 * khvr: standing in the world with View > Draw what the game's camera does not see, an enemy
 * outside the game camera's view cone (Ov107_IsBehindView) is drawn as one inside it: you look
 * around more than the DS's camera does. The AI's own use of the cone stays as it is. */

extern void DispatchObjectCallbacks(int this_, int arg1);
extern void *Ov107_GetActorManager(void);
extern int Ov107_IsBehindView(int ctx, void *extra);
#include "khvr_host.h"

typedef struct {
    char pad[0x5c];
    unsigned bit0 : 1;
    unsigned enableSomething : 1;
    unsigned rest : 30;
} Obj9c;

typedef struct {
    unsigned short lowFlags : 8;
    unsigned short highFlags : 8;
} Field60;

void Ov107_AiState_DispatchModelCallbacks(void *self_, int flag) {
    char *self = (char *)self_;
    Obj9c *t = *(Obj9c **)(self + 0x9c);
    Field60 *f60 = (Field60 *)(self + 0x60);

    if (t == 0) return;

    t->enableSomething = (f60->lowFlags & 0x80) != 0;

    if (flag != 0) {
        DispatchObjectCallbacks(*(int *)(self + 0x9c), flag);
        return;
    }

    if (f60->lowFlags & 0x20) {
        DispatchObjectCallbacks(*(int *)(self + 0x9c), flag);
        return;
    }

    {
        void *thr = Ov107_GetActorManager();
        int ctx = *(int *)thr;
        int result = khvr_host_draw_all() ? 0 : Ov107_IsBehindView(ctx, self + 0x74);
        DispatchObjectCallbacks(*(int *)(self + 0x9c), result);
    }
}
