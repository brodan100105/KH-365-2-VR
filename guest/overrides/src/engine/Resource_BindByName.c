#pragma thumb on
/* Port version of Resource_BindByName: the same, with the first binds logged (handle address,
 * resource pointer), so a bad font met later (see the port's NNS_G2dFontFindGlyphIndex) can be
 * matched to the bind it came from. */
#include "khvr_host.h"

extern void MI_CpuFill8(void *dest, int val, int size);
extern int Archive_LoadFile(char *name, int kind);
extern void NNS_G2dFontInitUTF16(int *a, int *b);

static int sBinds;

/* Zero a 0xc-byte handle, bind the Archive_LoadFile(name,0xe) resource at handle[2],
 * then run NNS_G2dFontInitUTF16 on it. */
void Resource_BindByName(void *pHandle, void *pName) {
    unsigned int *handle = (unsigned int *)pHandle;
    char *name = (char *)pName;
    MI_CpuFill8(handle, 0, 0xc);
    handle[2] = Archive_LoadFile(name, 0xe);
    NNS_G2dFontInitUTF16((int *)handle, (int *)handle[2]);
    if (sBinds < 32) {
        sBinds++;
        khvr_host_print(name);
        khvr_host_trace("font: bound at", (unsigned long)handle, (unsigned long)handle[0]);
    }
}
