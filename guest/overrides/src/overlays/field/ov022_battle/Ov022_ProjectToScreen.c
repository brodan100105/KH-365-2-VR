/* Projects a world position to screen coordinates (fx32, 8 pixels up); returns -1 when it is off
 * screen or behind the camera.
 *
 * khvr: only the lock-on sight (Ov022_UpdateSelectionMarker) places itself through here; its
 * target's world position also goes to the platform, which draws the sight in VR around the
 * target itself (the game's own sight is 2D sprites placed for its own camera). */

#include "nitro/types.h"
#include "nitro/fx_types.h"
#include "khvr_host.h"

typedef struct Ov022SelectionPoint {
    int x;
    int y;
} Ov022SelectionPoint;

extern int func_0201653c(const VecFx32 *world, int *screenX, int *screenY);
extern void VEC_Subtract(const VecFx32 *a, const VecFx32 *b, VecFx32 *out);
extern int VEC_Mag(const VecFx32 *vector);
extern void VEC_Normalize(const VecFx32 *source, VecFx32 *destination);
extern int VEC_DotProduct(const VecFx32 *a, const VecFx32 *b);
extern VecFx32 data_020475ac[];
extern VecFx32 data_020475c4;

int Ov022_ProjectToScreen(const VecFx32 *position, Ov022SelectionPoint *point)
{
    int projectionResult;
    int screenX;
    int screenY;
    VecFx32 toPosition;
    VecFx32 viewDirection;
    u32 cameraAddress = (u32)data_020475ac;
    u32 cameraTargetAddress = (u32)&data_020475c4;

    khvr_host_lock_on(position);
    projectionResult = func_0201653c(position, &screenX, &screenY);
    screenY -= 8;
    if (projectionResult != -1) {
        VEC_Subtract(position, (const VecFx32 *)cameraAddress, &toPosition);
        VEC_Subtract((const VecFx32 *)cameraTargetAddress,
                     (const VecFx32 *)cameraAddress, &viewDirection);
        if (VEC_Mag(&toPosition) != 0) {
            VEC_Normalize(&toPosition, &toPosition);
        }
        if (VEC_Mag(&viewDirection) != 0) {
            VEC_Normalize(&viewDirection, &viewDirection);
        }
        if (VEC_DotProduct(&viewDirection, &toPosition) < 0) {
            projectionResult = -1;
        }
    }

    point->x = screenX << 12;
    point->y = screenY << 12;
    return projectionResult;
}

