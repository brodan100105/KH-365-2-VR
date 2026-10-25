/* Port version of Ov029_AcquireOverlaySlot (decomp: src/overlays/system/ov029/), the player
 * overlay slot allocator. Its init hooks are the functions at the four slots' load addresses
 * (0x020b3220 for overlays 30-49, 0x020b5a20 for 50-68, 0x020b8100 for 70-87, 0x020ba7c0 for
 * 88-104): on the DS the hook runs whichever overlay is loaded there, but the decomp names each
 * address after one overlay (ov030, ov050, ov070, ov088), so the port calls the init of the
 * overlay it just loaded instead. When no slot is free the ROM indexes the hook table with -1;
 * the port skips the hook and logs it.
 *
 * The four slots are one array on the DS (0x020b3200-0x020b320f) that the decomp splits into
 * data_ov029_020b3200 (12 bytes: slots 0-2) and data_ov029_020b320c (slot 3 onwards), and the
 * port's linker leaves a gap between the two, so every slot goes through Slot() here and in
 * Ov029_ReleaseOverlaySlot (the same override directory). */
#include "game/engine.h"
#include "khvr_host.h"

typedef struct {
    int overlayId[4];
} OverlayGroup;

extern OverlayGroup data_ov029_020b30b0[];
extern int data_ov029_020b3200[];
extern int data_ov029_020b320c[];

int *Ov029_Slot(int k)
{
    return k < 3 ? &data_ov029_020b3200[k] : &data_ov029_020b320c[k - 3];
}

#define SLOT0(n) extern void func_ov0##n##_020b3220(void);
#define SLOT1(n) extern void func_ov0##n##_020b5a20(void);
#define SLOT2(n) extern void func_ov0##n##_020b8100(void);
#define SLOT3(n) extern void func_ov##n##_020ba7c0(void);
#define SLOT3_0(n) extern void func_ov0##n##_020ba7c0(void);
SLOT0(30) SLOT0(31) SLOT0(32) SLOT0(33) SLOT0(34) SLOT0(35) SLOT0(36) SLOT0(37) SLOT0(38) SLOT0(39)
SLOT0(40) SLOT0(41) SLOT0(42) SLOT0(43) SLOT0(44) SLOT0(45) SLOT0(46) SLOT0(47) SLOT0(48) SLOT0(49)
SLOT1(50) SLOT1(51) SLOT1(52) SLOT1(53) SLOT1(54) SLOT1(55) SLOT1(56) SLOT1(57) SLOT1(58) SLOT1(59)
SLOT1(60) SLOT1(61) SLOT1(62) SLOT1(63) SLOT1(64) SLOT1(65) SLOT1(66) SLOT1(67) SLOT1(68)
SLOT2(70) SLOT2(71) SLOT2(72) SLOT2(73) SLOT2(74) SLOT2(75) SLOT2(76) SLOT2(77) SLOT2(78) SLOT2(79)
SLOT2(80) SLOT2(81) SLOT2(82) SLOT2(83) SLOT2(84) SLOT2(85) SLOT2(86) SLOT2(87)
SLOT3_0(88) SLOT3_0(89) SLOT3_0(90) SLOT3_0(91) SLOT3_0(92) SLOT3_0(93) SLOT3_0(94) SLOT3_0(95)
SLOT3_0(96) SLOT3_0(97) SLOT3_0(98) SLOT3_0(99) SLOT3(100) SLOT3(101) SLOT3(102) SLOT3(103) SLOT3(104)

#define CASE0(n) case n: func_ov0##n##_020b3220(); break;
#define CASE1(n) case n: func_ov0##n##_020b5a20(); break;
#define CASE2(n) case n: func_ov0##n##_020b8100(); break;
#define CASE3_0(n) case n: func_ov0##n##_020ba7c0(); break;
#define CASE3(n) case n: func_ov##n##_020ba7c0(); break;

static void RunOverlayInit(int id)
{
    switch (id) {
    CASE0(30) CASE0(31) CASE0(32) CASE0(33) CASE0(34) CASE0(35) CASE0(36) CASE0(37) CASE0(38) CASE0(39)
    CASE0(40) CASE0(41) CASE0(42) CASE0(43) CASE0(44) CASE0(45) CASE0(46) CASE0(47) CASE0(48) CASE0(49)
    CASE1(50) CASE1(51) CASE1(52) CASE1(53) CASE1(54) CASE1(55) CASE1(56) CASE1(57) CASE1(58) CASE1(59)
    CASE1(60) CASE1(61) CASE1(62) CASE1(63) CASE1(64) CASE1(65) CASE1(66) CASE1(67) CASE1(68)
    CASE2(70) CASE2(71) CASE2(72) CASE2(73) CASE2(74) CASE2(75) CASE2(76) CASE2(77) CASE2(78) CASE2(79)
    CASE2(80) CASE2(81) CASE2(82) CASE2(83) CASE2(84) CASE2(85) CASE2(86) CASE2(87)
    CASE3_0(88) CASE3_0(89) CASE3_0(90) CASE3_0(91) CASE3_0(92) CASE3_0(93) CASE3_0(94) CASE3_0(95)
    CASE3_0(96) CASE3_0(97) CASE3_0(98) CASE3_0(99) CASE3(100) CASE3(101) CASE3(102) CASE3(103) CASE3(104)
    default:
        khvr_host_trace("player overlay init: no hook for overlay", (unsigned long)id, 0);
        break;
    }
}

int Ov029_AcquireOverlaySlot(int proc, int group)
{
    int loaded = -1;
    int k = 3; /* walks the slots from 3 down to 0 */

    do {
        if (*Ov029_Slot(k) == -1 && data_ov029_020b30b0[group].overlayId[k] != -1) {
            loaded = data_ov029_020b30b0[group].overlayId[k];
            *Ov029_Slot(k) = loaded;
            break;
        }
        k--;
    } while (k >= 0);

    LoadOverlaySync(0, loaded);
    if (k >= 0) RunOverlayInit(loaded);
    else khvr_host_trace("player overlay slots: none free for group", (unsigned long)group, 0);
    return loaded;
}
