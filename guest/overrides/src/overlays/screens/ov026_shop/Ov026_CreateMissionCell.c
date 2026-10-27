/* Port version of Ov026_CreateMissionCell: the decomp passes &xform as the transform, which on
 * ARM is xform followed by the trailing argument words; on wasm those sit in a separate buffer,
 * so the transform is gathered into an array first. */
extern int  VeneerTo_SlotTable_AddEntry(int *mgr, unsigned int res, int slot);
extern void Slot_SetMode2Bit(int mgr, int obj, int a);
extern void Slot_ClearFlagBit1(int mgr, int obj);
extern void Slot_SetVisible(int mgr, int obj, int a);
extern void Slot_SetPosition(int mgr, int obj, int *xform);

int Ov026_CreateMissionCell(int *mgr, unsigned int res, int slot, int xform, ...) {
    int t[4];
    __builtin_va_list ap;
    int i;
    int obj = VeneerTo_SlotTable_AddEntry(mgr, res, slot);
    t[0] = xform;
    __builtin_va_start(ap, xform);
    for (i = 1; i < 4; i++) t[i] = __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    Slot_SetMode2Bit((int)mgr, obj, 1);
    Slot_ClearFlagBit1((int)mgr, obj);
    Slot_SetVisible((int)mgr, obj, 0);
    Slot_SetPosition((int)mgr, obj, t);
    return obj;
}
