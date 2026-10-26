/* Port version of Ov114_AiStep_QueueStoredActionIfActive (decomp: src/overlays/enemies/
 * ov114_enemy_shadow/): the same AI step, plus a boot-log line now and then while a Shadow waits
 * to become active, with the state that decides it: the flags at +0x60 (the low byte, bit 0 =
 * active, is copied from the high byte by the pose message), the current, pending and stored
 * actions (+0x1c6, +0x1c7, +0x1c9), the mode (+0x50) and the throttle count (+0x30c). */

#include "game/actor.h"
#include "game/ai_task.h"
#include "khvr_host.h"

extern int SetIndexedSlot();

struct Obj {
    AI_TASK_FIELDS(Actor *)
};

void Ov114_AiStep_QueueStoredActionIfActive(struct Obj *this) {
    static unsigned int waits;
    Actor *s = *this->pState;
    if ((unsigned)(s->flags60.raw << 24) >> 24 & 1) {
        s->nextState = s->field_1c9;
        SetIndexedSlot(this, this->slot, 0);
        khvr_host_trace("shadow: active", (unsigned long)s, s->flags60.raw);
        return;
    }
    if (waits++ % 600 == 0) {
        const unsigned char *b = (const unsigned char *)s;
        khvr_host_trace("shadow: waiting (flags60, actions 1c6/1c7/1c9)", s->flags60.raw,
                        (unsigned long)b[0x1c6] << 16 | (unsigned long)b[0x1c7] << 8 | b[0x1c9]);
        khvr_host_trace("shadow: waiting (mode +0x50, throttle +0x30c)", *(const int *)(b + 0x50),
                        *(const int *)(b + 0x30c));
    }
}
