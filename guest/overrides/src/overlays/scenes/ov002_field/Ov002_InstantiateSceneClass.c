/* Port version of Ov002_InstantiateSceneClass (the decomp's passes &first as the argument list,
 * which on ARM is the start of the caller's argument words; on wasm `first` is a local and the
 * rest sit in a separate buffer). The constructor's arguments are gathered into one array. */
extern int InstantiateClass(void *classDesc, void *va);
extern int data_ov002_0207e830;
extern int data_ov002_0207f600;

int Ov002_InstantiateSceneClass(int first, ...) {
    int args[8];
    __builtin_va_list ap;
    int i;
    args[0] = first;
    __builtin_va_start(ap, first);
    for (i = 1; i < 8; i++) args[i] = __builtin_va_arg(ap, int); /* extra words are never read */
    __builtin_va_end(ap);
    *(int *)((char *)&data_ov002_0207f600 + 4) = InstantiateClass(&data_ov002_0207e830, args);
    return 1;
}
