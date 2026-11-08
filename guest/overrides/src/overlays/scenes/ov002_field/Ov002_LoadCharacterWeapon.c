/* VR port: the decomp's Ov002_LoadCharacterWeapon, telling the platform which weapon (the row of
 * the character's weapon table) each character has as a scene loads it (khvr_host_weapon_row),
 * so the Keyblade kept for your hand is the one equipped, before the game draws it. */
#include "khvr_host.h"
extern int OS_SPrintf(char *dst, const char *fmt, ...);
extern void *Archive_LoadFile(void *path, int heap);
extern void Ov002_PackTableRow(void *dst, void *file, int slot, int arg);
extern void NNSi_FndFreeFromDefaultHeap(void *p);

extern char *data_02042a70[];
extern char gOv002BaChWpPathFmt[];

/* Load one character's weapon model and pack it into the caller's table row.
 * gOv002BaChWpPathFmt is "ba/ch/%s/wp.b.z" and data_02042a70 is the table of
 * two-letter character codes -- "ro" "ri" "go" "ax" "xi" "la" "sa" "so"
 * (Roxas, Riku, ..., Axel, Xion, Larxene, Saix, Sora) -- so `slot` indexes the
 * roster.  The archive file is transient: packed, then freed straight away.
 *
 * Takes THREE parameters.  Ghidra shows four; only r0-r2 are ever set up. */
void Ov002_LoadCharacterWeapon(void *dst, int slot, int arg) {
    char path[128];
    void *file;

    OS_SPrintf(path, gOv002BaChWpPathFmt, data_02042a70[slot]);
    file = Archive_LoadFile(path, 6);
    Ov002_PackTableRow(dst, file, slot, arg);
    khvr_host_weapon_row(slot, arg);
    NNSi_FndFreeFromDefaultHeap(file);
}
