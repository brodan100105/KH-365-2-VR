#!/usr/bin/env python3
"""Compares the guest's data layout with the DS's.

    guest/check_data_layout.py [build/guest/game.map]

Code often reads past one data symbol into the next (the decomp splits tables the original kept
whole), so two symbols that are adjacent on the DS must stay adjacent in the port. For every pair
of consecutive initialized data symbols of one module (config/arm9/**/symbols.txt), this checks
that the port puts the second where the DS does relative to the first, and prints the pairs that
differ. Pairs that straddle sections or modules are not compared."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CFG = ROOT / "decomp" / "config" / "arm9"


def ds_symbols():
    out = {}
    for f in CFG.rglob("symbols.txt"):
        mod = f.parent.name if f.parent != CFG else "main"
        for line in f.read_text().splitlines():
            m = re.match(r"(\S+) kind:data\((\w+)\) addr:0x([0-9a-f]+)", line)
            if m:
                out.setdefault(mod, []).append((int(m.group(3), 16), m.group(1)))
    return out


def port_symbols(map_path):
    out = {}
    for line in Path(map_path).read_text().splitlines():
        m = re.match(r"\s*([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+(\w+)$", line)
        if m:
            out[m.group(3)] = (int(m.group(1), 16), int(m.group(2), 16))
    return out


def main():
    port = port_symbols(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build" / "guest" / "game.map")
    bad = total = 0
    for mod, syms in sorted(ds_symbols().items()):
        syms.sort()
        for (a0, n0), (a1, n1) in zip(syms, syms[1:]):
            if n0 not in port or n1 not in port:
                continue
            (p0, s0), (p1, _) = port[n0], port[n1]
            # Only pairs the port keeps together: the first one's size covers the DS gap.
            if s0 != a1 - a0:
                continue
            total += 1
            if p1 - p0 != a1 - a0:
                bad += 1
                if bad <= 40:
                    print("%-6s %s (0x%08x, %d bytes) -> %s: DS +0x%x, port %+#x" % (mod, n0, a0, s0, n1, a1 - a0, p1 - p0))
    print("%d of %d adjacent data pairs are not adjacent in the port" % (bad, total))


if __name__ == "__main__":
    main()
