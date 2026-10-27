/* Port version of Ov029_ReleaseOverlaySlot: unloads an overlay and frees its slot. The slots go
 * through Ov029_Slot (Ov029_AcquireOverlaySlot.c, same directory), since the decomp's two
 * symbols for the one slot array are not adjacent in the port. */

#include "game/engine.h"

extern int *Ov029_Slot(int k);

void Ov029_ReleaseOverlaySlot(int param_1)
{
    int i;
    UnloadOverlaySync(0, param_1);
    for (i = 0; i < 4; i++) {
        if (param_1 == *Ov029_Slot(i)) {
            *Ov029_Slot(i) = -1;
            return;
        }
    }
}
