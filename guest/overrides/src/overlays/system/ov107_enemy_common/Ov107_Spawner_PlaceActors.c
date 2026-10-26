/* Port copy of the decomp's Ov107_Spawner_PlaceActors with boot-log traces (the count and mode
 * of each placement, and each actor's spawn handler in mode 0), to find why placed enemies do not
 * wake. */
/* Ov107_Spawner_PlaceActors -- broadcast a positioning update to every entry of a
 * target list, per self->mode:
 *   0: no repositioning, just refresh + notify each entry.
 *   1: scatter each entry to a random point in the square
 *      [-spread/2, +spread/2] around self->pos on x and z (y is copied
 *      through unchanged).
 *   2/3/4: line the entries up along a world axis (X/Y/Z respectively),
 *      spaced by a step derived from func_02020400(self->spread, total);
 *      modes 2 and 4 start half a spread back from self->pos, mode 3 starts
 *      exactly at self->pos.
 *   5: arrange the entries evenly around a circle of radius self->spread/2
 *      centred on self->pos, using the angle table data_0203d210.
 * Bails out immediately if the target list's count (list->total) is 0,
 * regardless of mode.
 *
 * MATCH NOTE (mode 1): the range width is computed once before the loop with
 * RangeWidth(), a subtraction on its own parameter. That is the only form found
 * that gives the ROM's `add r6, r7, r7`: `half * 2`, `half + half` and a fresh
 * `span = hi - lo` all emit `lsl`, and a width computed inside the loop's
 * roll helper hoists `-half` ahead of the width. Tentative reconstruction of
 * how the original split its random-in-range routine. */

#include "nitro/types.h"
#include "nitro/fx_types.h"
#include "khvr_host.h"

typedef struct TargetList {
    u8  pad00[0x20];
    int total;      /* +0x20 */
} TargetList;

typedef struct Entry Entry;
struct Entry {
    u8 pad000[0x1cc];
    void (*onEvent)(Entry *self, int mode); /* +0x1cc */
};

typedef struct Self {
    u8      pad000[0x4a];
    u16     mode;        /* +0x4a */
    u8      pad04c[0x104 - 0x4c];
    VecFx32 pos;          /* +0x104 */
    u8      pad110[0x118 - 0x110];
    int     spread;       /* +0x118 */
} Self;

extern void        *List_First(void *list);
extern void        *List_Next(void *list);
extern void         Ov107_MoveNodeAndRelayout(char *node, VecFx32 *v);
extern int           RandNextScaled(int range);
extern void          ScaleVec3Fx12(int factor, VecFx32 *src, VecFx32 *dst);
extern void          VEC_Add(VecFx32 *a, VecFx32 *b, VecFx32 *out);
extern int           func_02020400(int a, int b);

extern VecFx32      data_02042270;
extern VecFx32      data_02042264;
extern VecFx32      data_02042258;
extern const short   data_0203d210[];

#define FX_MUL(a, b) ((int)(((long long)(a) * (b) + 0x800) >> 12))
#define ANGLE_TO_INDEX(angle) \
    ((int)(((unsigned)(((long long)(int)(unsigned)(angle) * 0x28be60db9391LL + \
           0x80000000000LL) >> 0x20) << 4) >> 0x10) >> 4)

/* Width of the range [lo, hi]. */
static inline int RangeWidth(int lo, int hi)
{
    hi -= lo;
    return hi;
}

/* Random value in [lo, lo + |span|]. */
static inline int RandSpan(int lo, int span)
{
    return lo + RandNextScaled((span < 0 ? -span : span) + 1);
}

void Ov107_Spawner_PlaceActors(Self *self, TargetList *list)
{
    int total;
    int half;
    u16 mode;
    void *node;

    total = list->total;
    half = self->spread >> 1;
    if (total != 0) khvr_host_trace("spawner: placing (count, mode)", total, self->mode);
    if (total == 0)
        return;

    mode = self->mode;
    switch (mode) {
    case 0: {
        node = List_First(list);
        while (node != 0) {
            Entry *entry = *(Entry **)node;
            Ov107_MoveNodeAndRelayout((char *)entry, &self->pos);
            khvr_host_trace("spawner: placed actor (address, spawn handler)", (unsigned long)entry,
                            (unsigned long)entry->onEvent);
            if (entry->onEvent)
                entry->onEvent(entry, 0);
            node = List_Next(list);
        }
        break;
    }
    case 1: {
        Entry *entry;
        VecFx32 pos;
        node = List_First(list);
        if (node != 0) {
            int span = RangeWidth(-half, half);

            do {
                entry = *(Entry **)node;
                pos.x = self->pos.x + RandSpan(-half, span);
                pos.y = self->pos.y;
                pos.z = self->pos.z + RandSpan(-half, span);

                Ov107_MoveNodeAndRelayout((char *)entry, &pos);
                if (entry->onEvent)
                    entry->onEvent(entry, 0);
                node = List_Next(list);
            } while (node != 0);
        }
        break;
    }
    case 2:
    case 3:
    case 4: {
        VecFx32 start;
        VecFx32 step;
        int factor;

        if (mode == 2) {
            ScaleVec3Fx12(-half, &data_02042270, &start);
            VEC_Add(&self->pos, &start, &start);
            factor = func_02020400(self->spread, total);
            ScaleVec3Fx12(factor, &data_02042270, &step);
        } else if (mode == 3) {
            start = self->pos;
            factor = func_02020400(self->spread, total);
            ScaleVec3Fx12(factor, &data_02042264, &step);
        } else if (mode == 4) {
            ScaleVec3Fx12(-half, &data_02042258, &start);
            VEC_Add(&self->pos, &start, &start);
            factor = func_02020400(self->spread, total);
            ScaleVec3Fx12(factor, &data_02042258, &step);
        }

        node = List_First(list);
        while (node != 0) {
            Entry *entry = *(Entry **)node;
            Ov107_MoveNodeAndRelayout((char *)entry, &start);
            VEC_Add(&start, &step, &start);
            Ov107_MoveNodeAndRelayout((char *)entry, &start);
            if (entry->onEvent)
                entry->onEvent(entry, 0);
            node = List_Next(list);
        }
        break;
    }
    case 5: {
        node = List_First(list);
        if (node != 0) {
            Entry *entry;
            int angle = 0;
            do {
                entry = *(Entry **)node;
                int raw = func_02020400(angle, total);
                int idx = ANGLE_TO_INDEX(raw);
                int cosv = data_0203d210[idx * 2];
                int sinv = data_0203d210[idx * 2 + 1];
                VecFx32 pos;
                pos.x = self->pos.x + FX_MUL(cosv, half);
                pos.y = self->pos.y;
                pos.z = self->pos.z + FX_MUL(sinv, half);
                Ov107_MoveNodeAndRelayout((char *)entry, &pos);
                if (entry->onEvent)
                    entry->onEvent(entry, 0);
                angle += 0x6488;
                node = List_Next(list);
            } while (node != 0);
        }
        break;
    }
    default:
        break;
    }
}
