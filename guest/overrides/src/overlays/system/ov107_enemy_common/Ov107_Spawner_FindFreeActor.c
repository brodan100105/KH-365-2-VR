/* Port copy of the decomp's Ov107_Spawner_FindFreeActor with boot-log traces: why each
 * candidate actor is or is not taken for a spawner slot. */
/* Finds an idle actor of the slot's kind (and sub-kinds) that can be spawned within the kind's
 * limit, counting it; returns it or 0. */

#include "nitro/types.h"
#include "khvr_host.h"

typedef struct {
    u8 f0;
    u8 f1;
    signed char f2;
    signed char f3;
} Entry;

extern void *List_First(void *list);
extern void *List_Next(void *list);

int Ov107_Spawner_FindFreeActor(int self, int index)
{
    int t4 = *(int *)(self + 4);
    if (t4 == 0) {
        khvr_host_trace("spawner: no owner for slot", index, 0);
        return 0;
    }

    u8 key = *(u8 *)(self + index * 12 + 0x50);
    int t5 = *(int *)(t4 + 4);

    if (t5 != 0) {
        u8 *arr9c = *(u8 **)(t5 + 0x9c);
        u8 val = arr9c[key];
        if (val != 0) {
            u8 *arra0 = *(u8 **)(t5 + 0xa0);
            u8 cur = arra0[key];
            if (cur >= val) {
                khvr_host_trace("spawner: kind at its live limit (slot, count)", index, cur);
                return 0;
            }
        }
    }

    {
        void *raw = List_First((void *)(t4 + 0x80));
        void *node = !raw ? 0 : *(void **)raw;

        while (node != 0) {
            int n = (int)node;
            unsigned int flags60 = (unsigned int)(*(u16 *)(n + 0x60) << 24) >> 24;

            khvr_host_trace("spawner: candidate (flags60, 1ac | 2e8 << 16)", *(u16 *)(n + 0x60),
                            *(u16 *)(n + 0x1ac) | (unsigned)*(int *)(n + 0x2e8) << 16);
            khvr_host_trace("spawner: candidate (2d4, kind 19c-19e | wanted f1-f3 << 16)",
                            *(int *)(n + 0x2d4),
                            (*(u8 *)(n + 0x19c) | *(u8 *)(n + 0x19d) << 4 | *(u8 *)(n + 0x19e) << 8) |
                            ((*(Entry **)(t5 + 0x94))[key].f1 | ((*(Entry **)(t5 + 0x94))[key].f2 & 15) << 4 |
                             ((*(Entry **)(t5 + 0x94))[key].f3 & 15) << 8) << 16);
            if (flags60 & 1) goto loopend;
            if (!(flags60 & 0x80)) goto loopend;
            if (!(*(u16 *)(n + 0x1ac) & 0x80)) goto loopend;
            if (*(int *)(n + 0x2e8) > 0) goto loopend;
            if (*(int *)(n + 0x2d4) != 0) goto loopend;

            {
                Entry *ip = *(Entry **)(t5 + 0x94);

                if (*(u8 *)(n + 0x19c) != ip[key].f1) goto loopend;

                if (ip[key].f2 != 0 && *(u8 *)(n + 0x19d) != ip[key].f2) goto loopend;

                if ((*(Entry **)(t5 + 0x94))[key].f3 != 0 &&
                    *(u8 *)(n + 0x19e) != (*(Entry **)(t5 + 0x94))[key].f3) goto loopend;
            }

            {
                u8 *arra0 = *(u8 **)(t5 + 0xa0);
                arra0[key] = arra0[key] + 1;
            }
            khvr_host_trace("spawner: found actor for slot", index, n);
            return n;

        loopend:
            raw = List_Next((void *)(t4 + 0x80));
            node = !raw ? 0 : *(void **)raw;
        }
    }

    khvr_host_trace("spawner: no free actor for slot", index, 0);
    return 0;
}
