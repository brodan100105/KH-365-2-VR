/* The MobiClip run/level table for format variant 1 (decomp: ov024's .rodata). The delink cut it
 * into three symbols that are contiguous on the DS; they are put back together here for the host
 * decoder in movie.cpp. */
#include <string.h>

_Static_assert(4096 * 2 + 256 == sizeof(unsigned short) * (76 + 8 + 4012) + 256, "variant 1 table pieces");

#include "../../decomp/src/overlays/system/ov024_mobiclip/data/ov024_vlc_variant1_020886c4.c"

static unsigned char sTable[4096 * 2 + 256];
static int sBuilt;

const void *khvr_mobiclip_vlc1(void)
{
    if (!sBuilt) {
        sBuilt = 1;
        memcpy(sTable, data_ov024_020886c4, sizeof data_ov024_020886c4);
        memcpy(sTable + sizeof data_ov024_020886c4, data_ov024_0208875c, sizeof data_ov024_0208875c);
        memcpy(sTable + sizeof data_ov024_020886c4 + sizeof data_ov024_0208875c, &data_ov024_0208876c,
               sizeof data_ov024_0208876c);
    }
    return sTable;
}
