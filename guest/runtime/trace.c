/* Traced copies of two tiny engine functions (exclude.txt drops the decomp's): the boot log
 * records every scene request and every script command table installed or removed, by name, so
 * a scene that stalls can be followed back to where it came from. Same behaviour otherwise. */
#include "khvr_host.h"

extern int data_020425ec[];  /* the action-script command tables, by opcode */
extern char data_0204bda4[]; /* the scene-control record: pending id at +0x10, its arg at +0x14 */

void StoreGlobalArrayEntry(int index, int value)
{
    data_020425ec[index] = value;
    khvr_host_trace("command table", (unsigned long)index, (unsigned long)value);
}

void Scene_RequestPending(int arg0, int arg1)
{
    *(int *)(data_0204bda4 + 0x10) = arg0;
    *(int *)(data_0204bda4 + 0x14) = arg1;
    khvr_host_trace("scene request", (unsigned long)arg0, (unsigned long)arg1);
}
