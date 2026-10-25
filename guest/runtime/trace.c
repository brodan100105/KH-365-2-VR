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

/* The two steps that pick and load a script from a packed archive, logged: a name that is not in
 * the archive's name table, and every script loaded through an archive handle (which entry, its
 * name, how many bytes and its first words). Same behaviour otherwise. */
extern int OS_SPrintf(char *pszDest, const char *pszFormat, ...);
extern int StrNCaseCmp(const signed char *a, const signed char *b, int n);
extern int FileLoader_LoadSync(const void *data, void *buffer, int size);
extern int Script_Start(char *o, const char *name, void *vm);

static unsigned char *ArchiveNames(unsigned char *header)
{
    unsigned short count = *(unsigned short *)(header + 2) & 0x1ff;
    return header + 0x10 + ((count + 1) / 2) * 4 + count * 4;
}

static void CopyName(char *dst, const unsigned char *src)
{
    int i;
    for (i = 0; i < 8 && src[i] >= 0x20 && src[i] < 0x7f; i++) dst[i] = (char)src[i];
    dst[i] = 0;
}

static int sArchiveTraces;

int MsgArchive_FindEntryByName(unsigned char *header, const char *name)
{
    int i, len = 0;
    unsigned short word = *(unsigned short *)(header + 2), count = word & 0x1ff;
    unsigned char *p = ArchiveNames(header);
    while (name[len]) len++;
    for (i = 0; i < count; i++, p += 8) {
        if (StrNCaseCmp((const signed char *)p, (const signed char *)name, len) == 0) return i;
    }
    if (sArchiveTraces < 48) {
        char line[160], first[4][9];
        int k;
        sArchiveTraces++;
        for (k = 0; k < 4; k++) first[k][0] = 0;
        if (word & 0x8000)
            for (k = 0; k < 4 && k < count; k++) CopyName(first[k], ArchiveNames(header) + k * 8);
        OS_SPrintf(line, "archive: \"%s\" not found in 0x%08x (%d entries, names %s: %s %s %s %s)", name,
                   (unsigned int)header, count, (word & 0x8000) ? "yes" : "no", first[0], first[1],
                   first[2], first[3]);
        khvr_host_print(line);
    }
    return -1;
}

int Stream_DecodeIntoStagingBuffer(char *o, int a, int c, int d)
{
    int n;
    *(int *)(o + 0x590) = 0x8000;
    n = FileLoader_LoadSync((const void *)a, o + 0x594, *(int *)(o + 0x590));
    if (sArchiveTraces < 48) {
        char line[200], entry[9];
        unsigned int *w = (unsigned int *)(o + 0x594);
        entry[0] = 0;
        if ((unsigned int)a & 0x80000000) {
            unsigned char *header = (unsigned char *)((((unsigned int)a >> 7) & 0xfffffc) + 0x1ff8000);
            unsigned int index = (unsigned int)a & 0x1ff;
            unsigned short word = *(unsigned short *)(header + 2);
            if ((word & 0x8000) && index < (word & 0x1ffu)) CopyName(entry, ArchiveNames(header) + index * 8);
            OS_SPrintf(line, "script load: entry %d \"%s\" of 0x%08x, member \"%s\"", (int)index, entry,
                       (unsigned int)header, c ? (const char *)c : "");
            khvr_host_print(line);
            OS_SPrintf(line, "script load: %d bytes: %08x %08x %08x %08x", n, w[0], w[1], w[2], w[3]);
        } else {
            OS_SPrintf(line, "script load: \"%s\", member \"%s\", %d bytes: %08x %08x %08x %08x", (const char *)a,
                       c ? (const char *)c : "", n, w[0], w[1], w[2], w[3]);
        }
        sArchiveTraces++;
        khvr_host_print(line);
    }
    if (n < 0) {
        return 0;
    }
    if (n % 4 != 0) {
        n += 4 - n % 4;
    }
    *(int *)o = n;
    *(int *)(o + 8) = n;
    return Script_Start(o, (const char *)c, (void *)d);
}

/* Boot self-test of the game's printf (platform.cpp logs the result): its trailing arguments
 * must come through, which on wasm needs the va_start rewrite in guest/build_guest.py. Run
 * directly and through the field's script-name formatter, the call that went wrong (which
 * worked or not depending on where its caller kept the arguments). */
extern void Ov002_FormatResultLine(int reason, char *buffer);

static int Same(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

int khvr_printf_selftest(void)
{
    char out[32];
    OS_SPrintf(out, "%s %d %x %d", "ab", 12, 0x3f, -7);
    if (!Same(out, "ab 12 3f -7")) return 0;
    Ov002_FormatResultLine((int)"_s", out);
    return Same(out, "_s");
}
