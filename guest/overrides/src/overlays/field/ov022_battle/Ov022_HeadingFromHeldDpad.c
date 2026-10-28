/* Returns the heading the held D-pad points to, relative to the camera (the actor's held keys at
 * +0x1a): Up 0, Left 0x3fff, Down 0x8000, Right 0xbfff, and a diagonal the average of its two
 * (Up+Left 0x2000, Up+Right 0xe000); -1 when no direction is held. Left and Right come out one
 * short of a quarter turn because the sum starts from -1.
 *
 * VR port: while a direction is held, the player moves the way the thumbstick points
 * (khvr_host_move_heading) instead of the D-pad's eight. */

#include "khvr_host.h"

int Ov022_HeadingFromHeldDpad(int actor) {
    unsigned short f = *(unsigned short *)(actor + 0x1a);
    int r = -1;
    int cnt = 0;
    long analog;
    if ((f & 0x40) != 0) {
        r = 0;
        cnt++;
    } else if ((f & 0x80) != 0) {
        cnt++;
        r = 0x8000;
    }
    if ((f & 0x20) != 0) {
        r += 0x4000;
        cnt++;
    } else if ((f & 0x10) != 0) {
        if ((f & 0x40) != 0) r += 0x1c000;
        else r += 0xc000;
        cnt++;
    }
    if (cnt > 1) r >>= 1;
    if (r >= 0) {
        analog = khvr_host_move_heading((const void *)actor);
        if (analog >= 0) r = (int)analog;
    }
    return r;
}
