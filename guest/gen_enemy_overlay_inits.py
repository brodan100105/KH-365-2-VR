#!/usr/bin/env python3
"""Writes the port's Ov107_LoadEnemyOverlay (guest/overrides/...), which calls the enemy overlay it
just loaded by name instead of jumping to its load address.

On the DS the enemy loader starts an enemy overlay by calling its load address: the overlay's
first function sits there. In the port functions are table entries, not addresses, so the call
traps ("indirect call to a bad function", as heartless first appear). The enemy overlays are
ov114-ov301; for each, the function at the start of its .text (decomp: config/arm9/overlays) is
its entry. Run again if the decomp renames them."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(ROOT, "decomp", "config", "arm9", "overlays")
OUT = os.path.join(ROOT, "guest", "overrides", "src", "overlays", "system", "ov107_enemy_common",
                   "Ov107_LoadEnemyOverlay.c")
FIRST, LAST = 114, 301


def entry(n):
    d = os.path.join(CONFIG, "ov%03d" % n)
    start = int(re.search(r"\.text\s+start:(0x[0-9a-f]+)", open(os.path.join(d, "delinks.txt")).read()).group(1), 16)
    for line in open(os.path.join(d, "symbols.txt")):
        if "kind:function" in line and int(re.search(r"addr:(0x[0-9a-f]+)", line).group(1), 16) == start:
            return line.split()[0]
    sys.exit("ov%03d: no function at its start 0x%08x" % (n, start))


def return_type(name):
    """The entry's return type as the decomp defines it (they all take no parameters)."""
    for dirpath, _, files in os.walk(os.path.join(ROOT, "decomp", "src")):
        for f in files:
            if not f.endswith(".c"):
                continue
            m = re.search(r"^([A-Za-z_][\w \t\*]*?)\b%s\s*\(\s*void\s*\)\s*\{" % re.escape(name),
                          open(os.path.join(dirpath, f), errors="replace").read(), re.M)
            if m:
                return " ".join(m.group(1).split())
    return "void"


def main():
    names = {n: entry(n) for n in range(FIRST, LAST + 1)}
    decls = "\n".join("extern %s %s(void);" % (return_type(names[n]), names[n]) for n in names)
    cases = "\n".join("    case %d: %s(); break;" % (n, names[n]) for n in names)
    src = '''/* Port version of Ov107_LoadEnemyOverlay (decomp: src/overlays/system/ov107_enemy_common/),
 * written by guest/gen_enemy_overlay_inits.py. The ROM starts the enemy overlay it loads by calling
 * its load address (info->field_4), where the overlay's first function sits; the port calls that
 * function by name, as its functions are table entries, not addresses. Some of them return a value
 * the loader never reads. Everything else is as in the decomp. */

#include "nitro/types.h"
#include "game/engine.h"
#include "khvr_host.h"

extern int FS_StartOverlay(void *info);
extern void FS_EndOverlay(char *overlay);
extern int FS_UnloadOverlayImage(void *p);
extern int Math_DivMod(void *a, int b);
extern void OS_Terminate(void);

extern int data_ov107_020cb6a0;
extern int data_ov107_020cb610[6];
extern void *data_ov107_020cb5f8[6];
extern unsigned char data_ov107_020cb6a4[];

extern u32 OVERLAY_113_ID[1];
#define FS_OVERLAY_ID_ov113 ((int)(u32)&(OVERLAY_113_ID))

typedef struct { int w[6]; } Block24;

typedef struct {
    int field_0;
    void *field_4;
    void *field_8;
} Info;

%s

/* The entry of enemy overlay `id` (%d-%d). */
static void RunEnemyOverlayEntry(int id)
{
    switch (id) {
%s
    default:
        khvr_host_trace("enemy overlay without an entry", (unsigned long)id, 0);
        break;
    }
}

int Ov107_LoadEnemyOverlay(int idx, int base, Info *info) {
    if (data_ov107_020cb6a0 != 0) {
        Block24 ids = *(Block24 *)data_ov107_020cb610;
        Block24 ptrs = *(Block24 *)data_ov107_020cb5f8;
        char localInfo[0x2c];
        int i;

        data_ov107_020cb6a0 = 0;
        for (i = 0; i < 6; i++) {
            Loader_LoadOverlayInfo(localInfo, 0, ids.w[i]);
            Loader_LoadOverlayImage((int)localInfo);
            FS_StartOverlay(localInfo);
            *(char *)ptrs.w[i] = 0;
            FS_EndOverlay(localInfo);
            FS_UnloadOverlayImage(localInfo);
        }
    }

    {
        unsigned char *row = data_ov107_020cb6a4 + idx * 6;
        unsigned char b = row[base];
        int result;
        int r4;
        Loader_LoadOverlayInfo(info, 0, FS_OVERLAY_ID_ov113 + b + 1);
        result = Math_DivMod(info->field_8, 0x1e00);
        r4 = result + 1;
        if (base + r4 > 6) {
            OS_Terminate();
            return -1;
        }
        Loader_LoadOverlayImage((int)info);
        FS_StartOverlay(info);
        RunEnemyOverlayEntry(%d + b);
        return r4;
    }
}
''' % (decls, FIRST, LAST, cases, FIRST)
    open(OUT, "w").write(src)
    print("wrote %s (%d enemy overlays)" % (os.path.relpath(OUT, ROOT), len(names)))


if __name__ == "__main__":
    main()
