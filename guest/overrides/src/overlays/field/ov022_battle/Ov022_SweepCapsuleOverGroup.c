/* Ov022_SweepCapsuleOverGroup -- test a capsule against every member of a group.
 *
 * Nothing is tested while the actor carries the no-contact flag. Otherwise the
 * capsule's own direction is taken once, the group named by the capsule is
 * looked up, and every member that is present and open to contact is tested
 * against the capsule with its own radius.
 *
 * Any hit answers yes. Only the local player reports one: the contact is turned
 * into a hit record and handed on, and the actor keeps the contact point.
 *
 * VR port: Roxas's swings reach a little further in first person (khvr_host_reach_bonus), so
 * you need not stand right on top of an enemy.
 */

#include "nitro/types.h"
#include "nitro/fx_types.h"
#include "khvr_host.h"

struct Capsule {
    VecFx32 vecA;                /* 0x00 */
    VecFx32 vecB;                /* 0x0c */
    int nGroup;                  /* 0x18 */
    int nRadius;                 /* 0x1c */
};

struct GroupMember {
    u8 pad0000[4];
    struct GroupMember *pNext;   /* 0x04 */
    u8 pad0008[0xa];
    u16 nFlags;                  /* 0x12 */
};

struct ContactPoint {
    VecFx32 vecPos;              /* 0x00 */
    int nRadius;                 /* 0x0c */
};

struct HitRecord {
    VecFx32 vecPos;              /* 0x00 */
};

struct Actor {
    unsigned int nFlags;         /* 0x0000 */
    u8 pad0004[0x26c4];
    VecFx32 vecImpact;           /* 0x26c8 */
};

extern void VEC_Subtract(VecFx32 *pA, VecFx32 *pB, VecFx32 *pOut);
extern int Ov002_GetSlotTableByte(int nGroup);
extern struct GroupMember *Ov002_List_GetWord(int nId);
extern struct ContactPoint *Ov002_TriggerEntryActive(struct GroupMember *pMember);
extern int Ov022_TestCapsuleReach(const VecFx32 *pPoint, int nRadius,
                               VecFx32 *pOut, struct Capsule *pCapsule);
extern int Session_GetLocalPlayerIndex(void);
extern void Ov022_BuildHitPush(struct HitRecord *pHit, void *pCtx,
                                VecFx32 *pContact, struct ContactPoint *pPoint,
                                VecFx32 *pDir);
extern void Ov022_ReportHit(struct Actor *pActor, void *pCtx,
                                struct GroupMember *pMember,
                                struct HitRecord *pHit, int nArg);

#define ACTOR_NO_CONTACT 0x10000
#define MEMBER_OPEN 0x40

int Ov022_SweepCapsuleOverGroup(struct Actor *pActor, struct Capsule *pCap, void *pCtx)
{
    struct HitRecord hit;
    VecFx32 vecDir;
    VecFx32 vecContact;
    struct GroupMember *pMember;
    struct ContactPoint *pPoint;
    int bHit;

    bHit = 0;
    if ((pActor->nFlags & ACTOR_NO_CONTACT) != 0) {
        return 0;
    }
    VEC_Subtract(&pCap->vecB, &pCap->vecA, &vecDir);
    pMember = Ov002_List_GetWord((u16)Ov002_GetSlotTableByte(pCap->nGroup));
    if (pMember != 0) {
        do {
            pPoint = Ov002_TriggerEntryActive(pMember);
            if (pPoint != 0 && (pMember->nFlags & MEMBER_OPEN) != 0
                && Ov022_TestCapsuleReach(&pPoint->vecPos,
                                       pPoint->nRadius + (int)khvr_host_reach_bonus(pActor),
                                       &vecContact, pCap) != 0) {
                bHit = 1;
                if (Session_GetLocalPlayerIndex() == 0 && pCtx != 0) {
                    Ov022_BuildHitPush(&hit, pCtx, &vecContact, pPoint,
                                        &vecDir);
                    Ov022_ReportHit(pActor, pCtx, pMember, &hit, 0);
                    pActor->vecImpact = vecContact;
                }
            }
            pMember = pMember->pNext;
        } while (pMember != 0);
    }
    return bHit;
}
