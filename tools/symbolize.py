#!/usr/bin/env python3
"""Turns the backtraces in a boot log (khvr_boot.log) into function names.

    tools/symbolize.py khvr_boot.log libkhdaysvr.so

The headset's log has library offsets (`libkhdaysvr.so+0x1234`); the unstripped library of the
same build is the `khdays-vr-symbols` artifact of the CI run that built the APK. The game's
functions keep the decomp's names (with wasm2c's `w2c_khdays_` prefix).
"""
import re
import shutil
import subprocess
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    log, lib = sys.argv[1], sys.argv[2]
    tool = shutil.which("llvm-addr2line") or shutil.which("llvm-addr2line-18") or shutil.which("addr2line")
    name = lib.rsplit("/", 1)[-1]
    for line in open(log, errors="replace"):
        m = re.search(re.escape(name) + r"\+0x([0-9a-f]+)", line)
        if not m:
            sys.stdout.write(line)
            continue
        out = subprocess.run([tool, "-f", "-C", "-e", lib, "0x" + m.group(1)], capture_output=True, text=True).stdout
        func = out.splitlines()[0] if out else "??"
        func = func.replace("w2c_khdays_", "").removesuffix("_0")
        sys.stdout.write(line.rstrip("\n") + "  " + func + "\n")


if __name__ == "__main__":
    main()
