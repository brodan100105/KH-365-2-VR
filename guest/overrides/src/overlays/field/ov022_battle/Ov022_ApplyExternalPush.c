/* ov022: fold an external push into the actor's step.
 *
 * Reads the push vector the actor is currently subject to, drops its downward
 * component while grounded, and adds it to the step. If the combined step and
 * the push are both longer than one unit the push is renormalised first, so a
 * strong push cannot stack without bound, and one input mode scales it down.
 * Only the horizontal part is committed.
 *
 * VR port: the player's room-scale steps (your steps in your room, khvr_host_room_step) join his
 * step here too, so the game's own mover takes them against its walls.
 */

#include "nitro/types.h"
#include "nitro/fx_types.h"
#include "khvr_host.h"

struct Actor {
    unsigned long long nFlags;   /* 0x000 */
    u8 pad008[0x1c];
    u32 nInputMask;              /* 0x024 */
    u8 pad028[0x470];
    VecFx32 vecStep;         /* 0x498 */
    u8 pad4a4[0x1c];
    VecFx32 *pPush;          /* 0x4c0 */
};

extern int VEC_Mag(const VecFx32 *pVec);
extern void VEC_Add(const VecFx32 *pA, const VecFx32 *pB,
                    VecFx32 *pOut);
extern void VEC_Normalize(const VecFx32 *pIn, VecFx32 *pOut);
extern void ScaleVec3Fx12(int nScale, const VecFx32 *pIn,
                          VecFx32 *pOut);

void Ov022_ApplyExternalPush(struct Actor *pActor)
{
    VecFx32 vecPush;
    VecFx32 vecSum;
    VecFx32 vecFlat;
    long room[2];

    khvr_host_room_step((const void *)pActor, room);
    pActor->vecStep.x += room[0];
    pActor->vecStep.z += room[1];
    if ((pActor->nFlags & (1ULL << 7)) != 0) {
        return;
    }
    if (pActor->pPush == 0) {
        return;
    }
    vecPush = *pActor->pPush;
    vecSum = pActor->vecStep;
    if ((pActor->nFlags & (1ULL << 10)) != 0) {
        return;
    }
    if (VEC_Mag(&vecPush) <= 0) {
        return;
    }
    if ((pActor->nInputMask & 4) != 0 && vecPush.y < 0) {
        vecPush.y = 0;
    }
    VEC_Add(&vecPush, &vecSum, &vecSum);
    if (VEC_Mag(&vecSum) > 0x1000 && VEC_Mag(&vecPush) > 0x1000) {
        VEC_Normalize(&vecPush, &vecPush);
    }
    if ((pActor->nInputMask & 2) != 0) {
        ScaleVec3Fx12(0x333, &vecPush, &vecPush);
    }
    vecFlat = vecPush;
    vecFlat.y = 0;
    VEC_Add(&pActor->vecStep, &vecFlat, &pActor->vecStep);
}
