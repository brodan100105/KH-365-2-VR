/* Updates the lock-on marker for the current selection type: projects the target to the screen and
 * draws the markers, or clears the selection when the target is gone.
 *
 * khvr: also reports the target to the platform (below). */

#include "nitro/types.h"
#include "game/engine.h"
#include "khvr_host.h"

typedef struct Ov022SelectionPoint {
    int x;
    int y;
} Ov022SelectionPoint;

typedef struct Ov022Type1Source {
    char pad_0000[0x21a];
    u16 row21a;
} Ov022Type1Source;

typedef struct Ov022TargetEntry {
    char pad_0000[0x12];
    u16 flags12;
} Ov022TargetEntry;

typedef struct Ov022Actor {
    u64 flags0;
} Ov022Actor;

typedef struct Ov022SelectionController {
    u32 flags0;
    u32 selectionFlags4;
    int type8;
    Ov022Type1Source *type1SourceC;
    void **type1List10;
    char pad_0014[4];
    Ov022TargetEntry *targetEntry18;
    void *type3Source1c;
    char pad_0020[0x1c];
    char type13State3c[0xac];
    char subsystemE8[0x148];
    int activationState230;
} Ov022SelectionController;

extern u16 gPadPressed;

extern Ov022SelectionController *NNSi_FndGetCurrentRootHeap(void);
extern void Ov002_MoveCaret(unsigned int row, int argument);
extern void Ov022_ClearWords124And128(void *subsystem);
extern void *Ov002_TriggerEntryActive(Ov022TargetEntry *entry);
extern void Ov022_SelectSlotAndRestartAnim(void *subsystem, Ov022TargetEntry *entry);
extern void Ov022_SetSelectionEnabled(int enabled);
extern int Ov022_ProjectToScreen(void *target, Ov022SelectionPoint *out);
extern void Ov022_DrawSelectionMarkers(void *state, const Ov022SelectionPoint *point);
extern void Ov022_UpdateUiSelectionMarker(void *subsystem,
                                const Ov022SelectionPoint *point);

void Ov022_UpdateSelectionMarker(void)
{
    Ov022SelectionPoint point;
    Ov022SelectionController *context = NNSi_FndGetCurrentRootHeap();
    void *target = 0;

    if (context->type8 == 0) {
        return;
    }
    if ((context->selectionFlags4 & 2) == 0) {
        return;
    }

    switch (context->type8) {
    case 1:
        if (Slot_EvalPackedParam(QueryActiveStateOrDelegate(), 0x53) != 0) {
            Ov002_MoveCaret(context->type1SourceC->row21a, 7);
        }
        target = (char *)*context->type1List10 + 4;
        Ov022_ClearWords124And128(context->subsystemE8);
        break;

    case 2: {
        void *resolvedTarget =
            Ov002_TriggerEntryActive(context->targetEntry18);
        if (resolvedTarget != 0) {
            target = resolvedTarget;
            if ((context->targetEntry18->flags12 & 0x80) == 0) {
                Ov022_SelectSlotAndRestartAnim(context->subsystemE8,
                                    context->targetEntry18);
            }
        }
        break;
    }

    case 3:
        target = (char *)context->type3Source1c + 0xf8;
        target = (char *)target + 0x800;
        Ov022_ClearWords124And128(context->subsystemE8);
        break;
    }

    {
        Ov022Actor *actor = (Ov022Actor *)GetEntryField20ByIndex(QueryActiveStateOrDelegate());
        if ((gPadPressed & 4) != 0 &&
            (actor->flags0 & 0x1000000ULL) == 0 &&
            (context->flags0 & 4) != 0) {
            Ov022_SetSelectionEnabled(0);
            return;
        }
    }

    if (target == 0) {
        return;
    }
    /* khvr: the target goes to the platform, on or off the game's screen, with its kind (1 and 3
     * enemies, 2 the field's entries: people to talk to, things to use) and whether it is locked
     * on. Standing in the world the headset draws the cursors at the target itself, so the game's
     * own (2D sprites placed for its camera, and the entries' corner marks drawn in front of it)
     * are left out. */
    khvr_host_lock_on(target, context->type8, (context->flags0 & 4) != 0);
    if (khvr_host_own_cursors()) {
        return;
    }
    if (Ov022_ProjectToScreen(target, &point) == -1) {
        return;
    }

    switch (context->type8) {
    case 1:
        Ov022_DrawSelectionMarkers(context->type13State3c, &point);
        return;

    case 2:
        if ((context->targetEntry18->flags12 & 0x80) != 0) {
            return;
        }
        Ov022_UpdateUiSelectionMarker(context->subsystemE8, &point);
        return;

    case 3:
        Ov022_DrawSelectionMarkers(context->type13State3c, &point);
        return;
    }
}

