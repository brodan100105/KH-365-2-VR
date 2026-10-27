#!/usr/bin/env python3
"""Builds the decompiled game (decomp/) as the port's 32-bit guest module.

    guest/build_guest.py [--discover] [--jobs N] [--out build/guest]

Pipeline (docs/ARCHITECTURE.md, "Why a guest module"):

  1. Sources: every C file of decomp/src and decomp/libs, minus guest/exclude.txt (the libraries'
     hand-written assembly, DS Protect, and the functions the platform replaces). A file in
     guest/overrides/ with the same relative path replaces the decomp's.
  2. Port fixes, applied to a copy under <out>/src (the submodule stays untouched): mechanical
     rewrites for what mwccarm accepts and clang does not, plus the per-file fixups in
     guest/fixups.json that --discover finds from compile and link errors.
  3. Storage: the uninitialized data (.bss) of every module, and all of DTCM, is generated as one
     contiguous block per module with every symbol at its original offset, from
     decomp/config/arm9/**/{delinks,symbols}.txt. Code reaches across symbol boundaries
     (`&data_027e0000 + 0x3ff8`), so the original layout is kept, not just the symbols.
  4. Compile for wasm32 (ILP32, like the DS) and link with wasm-ld. Functions nobody defines become
     imports the platform layer implements (or stubs out with a log line).
  5. wasm2c turns the module into C, which guest/postprocess.py adapts (I/O traps, lenient
     indirect calls, signature-mismatch adapters, per-thread stack pointer). The result is
     compiled into the APK by android/app/src/main/cpp/CMakeLists.txt.

--discover runs steps 1-4 repeatedly, recording new fixups in guest/fixups.json until the link is
clean. A normal build only applies the recorded fixups, so it is deterministic.
"""
import argparse
import concurrent.futures as cf
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GUEST = ROOT / "guest"
DECOMP = ROOT / "decomp"
FIXUPS = GUEST / "fixups.json"

# The guest's linear memory is the DS memory map. The guest's data starts at DTCM (0x027e0000):
# the first 128 KB are one generated block holding DTCM and the ARM7-shared page (0x027ff000) at
# their real addresses, since the SDK reaches both through literal addresses; the compiled data,
# .bss blocks and stack follow from 0x02800000. Below stays free for the platform: main RAM
# 0x02000000-0x027e0000 (the game's heap arenas), ITCM 0x01ff8000. Above: I/O 0x04000000,
# palettes 0x05000000, VRAM 0x06000000, OAM 0x07000000.
GLOBAL_BASE = 0x027e0000
LOW_END = 0x02800000
MEMORY_BYTES = 0x10000000          # 256 MB of linear memory
GUEST_STACK_BYTES = 0x100000       # the main thread's stack; other threads use their OS stacks

CFLAGS = [
    "--target=wasm32", "-ffreestanding", "-nostdinc", "-fno-builtin", "-O2", "-w",
    "-fno-strict-aliasing",  # decompiled code type-puns freely
    "-fwrapv",               # and relies on ARM's wrapping integer arithmetic
    # mwcc's ARM ABI aligns 64-bit integers (and doubles) to 4 bytes, wasm32 to 8: without this
    # every structure holding one after an odd word (OSContext, with the divider's u64
    # registers) would grow, and code that reaches its fields by offset would miss them.
    "-fpack-struct=4",
    "-DTARGET_KHVR=1",
    # What mwccarm accepted and modern clang rejects by default:
    "-Wno-error=int-conversion", "-Wno-error=implicit-function-declaration",
    "-Wno-error=return-type", "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=implicit-int",
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def load_fixups():
    if FIXUPS.exists():
        return json.loads(FIXUPS.read_text())
    return {"unstatic": {}, "function_not_data": {}, "data_not_function": {}, "weak": {}}


def save_fixups(fx):
    for k in fx:
        fx[k] = {f: sorted(set(v)) for f, v in sorted(fx[k].items())}
    FIXUPS.write_text(json.dumps(fx, indent=1, sort_keys=True) + "\n")


# --------------------------------------------------------------------------------------------
# 1. Sources
# --------------------------------------------------------------------------------------------

def read_patterns(path):
    out = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append(line)
    return out


def collect_sources():
    excludes = read_patterns(GUEST / "exclude.txt")
    srcs = []
    for top in ("src", "libs"):
        for p in sorted((DECOMP / top).rglob("*.c")):
            rel = p.relative_to(DECOMP).as_posix()
            # fnmatch's `*` also matches `/`, so `libs/*/asm_stubs/*` covers every depth.
            if any(fnmatch.fnmatch(rel, pat) for pat in excludes):
                continue
            srcs.append(rel)
    return srcs


# --------------------------------------------------------------------------------------------
# 2. Port fixes
# --------------------------------------------------------------------------------------------

RE_CLZ = re.compile(r"asm\s*\{\s*clz\s+(\w+)\s*,\s*(\w+)\s*\}")
RE_REGPIN = re.compile(r"\s+(?:asm|__asm__)\s*\(\s*\"r\d+\"\s*\)")
RE_OVL_DECL = re.compile(r"^[ \t]*extern[^;\n]*\bOVERLAY_\d+_ID\b[^;\n]*;[ \t]*$", re.M)
RE_OVL_ADDR = re.compile(r"&\s*(?:\(\s*OVERLAY_(\d+)_ID\s*\)|OVERLAY_(\d+)_ID\b)")
RE_OVL = re.compile(r"\bOVERLAY_(\d+)_ID\b")
# Linker-absolute SDK constants the decomp declares as functions (their address is the value).
SDK_CONSTANTS = {"SDK_SYS_STACKSIZE": 0, "SDK_IRQ_STACKSIZE": 0x800,
                 "SDK_SECTION_ARENA_DTCM_START": 0x027e0e60}
# mwcc lets a whole array be assigned through a cast: `*(int (*)[4])dst = *(int (*)[4])src;`
RE_ARRAY_ASSIGN = re.compile(
    r"\*\(int \(\*\)\[(\d+)\]\)\s*(\([^;=]+\)|[\w.>-]+)\s*=\s*\*\(int \(\*\)\[\d+\]\)\s*([^;]+);")


def redeclare(t, name, replacement, function):
    """Replaces the extern declaration of `name` (a data or a function declaration) with
    `replacement`, keeping any other names declared in the same statement."""
    n = re.escape(name)
    if function:
        pat = re.compile(r"\bextern\s+[^;{}()]*?\b%s\s*\([^;{}]*\)\s*;" % n)
        return pat.sub(replacement, t)
    def fix(m):
        stmt = m.group(0)
        if "," not in stmt:
            return replacement
        # `extern int a, *b, NAME, c[2];`: drop NAME, keep the rest with the first item's type.
        items = [i.strip() for i in stmt[len("extern"):].rstrip(";").split(",")]
        typ, items[0] = re.match(r"(.*?)([\s*]*\b\w+\b\s*(?:\[[^\]]*\])?)$", items[0], re.S).groups()
        keep = [i for i in items if re.sub(r"[\s*]|\[.*\]", "", i) != name]
        return ("extern %s %s; " % (typ.strip(), ", ".join(keep)) if keep else "") + replacement
    pat = re.compile(r"\bextern\s+[^;{}()]*?\b%s\b[^;{}()]*;" % n)
    return pat.sub(fix, t)


def config_symbols():
    """Every symbol name the decomp's config places at an address."""
    global _CONFIG_SYMBOLS
    if _CONFIG_SYMBOLS is None:
        _CONFIG_SYMBOLS = set()
        for f in (DECOMP / "config" / "arm9").rglob("symbols.txt"):
            for line in f.read_text().splitlines():
                if line.strip():
                    _CONFIG_SYMBOLS.add(line.split()[0])
    return _CONFIG_SYMBOLS


_CONFIG_SYMBOLS = None
_DS_DATA = None


def ds_data_addresses():
    """Initialized data symbols the decomp's config places, by name: (module, DS address).
    A name two modules both use (overlays share addresses) keeps the first."""
    global _DS_DATA
    if _DS_DATA is None:
        _DS_DATA = {}
        cfg = DECOMP / "config" / "arm9"
        for f in sorted(cfg.rglob("symbols.txt")):
            mod = f.parent.name if f.parent != cfg else "main"
            for line in f.read_text().splitlines():
                m = re.match(r"(\S+) kind:data\(\w+\) addr:0x([0-9a-f]+)", line)
                if m and m.group(1) not in _DS_DATA:
                    _DS_DATA[m.group(1)] = (mod, int(m.group(2), 16))
    return _DS_DATA


def is_data_file(rel):
    return "/data/" in rel


# The data files' definitions: `<type> name[dims] = ...` (the attribute goes before the `=`).
RE_DATA_DEF = re.compile(r"\b([A-Za-z_]\w*)((?:\s*\[[^\]\n]*\])*)(\s*=)")
RE_DATA_CONST = re.compile(r"^([ \t]*(?:static[ \t]+)?)const[ \t]+|(\*[ \t]*)const\b", re.M)


def ds_layout(text, strip_const=True):
    """Keeps a data file's definitions where the DS has them relative to each other: code reads
    past one symbol into the next (the decomp splits tables the original kept whole), and clang
    would align every array of 16 bytes or more to 16 and keep const ones in a separate segment.
    Each definition gets its DS address's alignment (which also stops that), and loses `const`
    (wasm memory is writable anyway) so the whole file is one run of .data."""
    addrs = ds_data_addresses()

    def attr(m):
        name = m.group(1)
        if name not in addrs or "__attribute__" in m.group(0):
            return m.group(0)
        if not strip_const:
            # Outside the data files, only file-scope definitions: a type (or a struct's closing
            # brace) at the start of the line, never an assignment inside a function.
            line = text[text.rfind("\n", 0, m.start()) + 1:m.start()]
            if not line or line[0] in " \t" or not re.match(r"[A-Za-z_}]", line) or "const" in line:
                return m.group(0)
        a = addrs[name][1]
        align = 4 if a % 4 == 0 else (2 if a % 2 == 0 else 1)
        # Its own .data section, also for an all-zero table (clang would put that in .bss).
        return "%s%s __attribute__((aligned(%d), section(\".data.%s\")))%s" % (
            name, m.group(2), align, name, m.group(3))

    if strip_const:
        text = RE_DATA_CONST.sub(lambda m: m.group(1) if m.group(1) is not None else m.group(2), text)
    return RE_DATA_DEF.sub(attr, text)


def data_file_order(rel, path):
    """Sort key that links the files defining DS data in DS order, each module's together."""
    addrs = ds_data_addresses()
    text = Path(path).read_text(errors="replace")
    found = [addrs[n] for n in re.findall(r"\b([A-Za-z_]\w*)(?:\s*\[[^\]\n]*\])*(?:\s*__attribute__\(\(.*?\)\))?\s*=", text)
             if n in addrs]
    return min(found) if found else None
# A function-local `static T data_02046d50;` that stands for the real global at that address
# (the original only kept the function's code): it becomes a block-scope extern declaration.
RE_STATIC_LOCAL = re.compile(r"\bstatic(\s+[^;=(){}]*?\b(data_[0-9a-f]{8})\b[^;=(){}]*;)")


# mwcc accepts post-incrementing through a cast, `*((T *)p)++` (read a T and step p past it);
# clang rejects the lvalue cast, so it is spelled out.
RE_LVALUE_CAST_INC = re.compile(r"\*\(\((\w+)\s*\*\)\s*(\w+)\)\+\+")
# A busy-wait loop with an empty body: `while (cond) {}` or `while (cond);` (not the tail of a
# do-while). The port takes interrupts only where the guest re-enables them, so a loop that
# spins with them already enabled would never see the reply it waits for.
RE_SPIN = re.compile(r"(?<![}\w])(\s)while\s*\(((?:[^;{}()]|\((?:[^;{}()]|\([^;{}()]*\))*\))*)\)\s*(?:\{\s*\}|;)")


def is_spin_wait(cond):
    """A poll of state another context changes: a volatile read or a status call, and nothing the
    loop itself changes. The hardware units the port answers at once (divider, square root,
    geometry engine) are left alone; they sit in hot code."""
    if re.search(r"\+\+|--|[^=!<>]=[^=]|<<=|>>=", cond):
        return False
    if re.search(r"0x0400|G3X_|CP_IsDivBusy", cond):
        return False
    return "volatile" in cond or re.search(r"\b(?!sizeof\b)[A-Za-z_]\w*\s*\(", cond) is not None


def spin_waits(text):
    def fix(m):
        if not is_spin_wait(m.group(2)):
            return m.group(0)
        return "%swhile (%s) { khvr_host_spin(); }" % (m.group(1), m.group(2))
    out = RE_SPIN.sub(fix, text)
    if out != text:
        out = "void khvr_host_spin(void);\n" + out
    return out


RE_VA_START_MACRO = re.compile(r"#define\s+va_start\(ap,\s*last\)\s*\(\(ap\)\s*=\s*\(char \*\)\(\(\(int\)&\(last\)\s*&\s*~3\)\s*\+\s*4\)\)")
RE_VA_INLINE = re.compile(r"\(void \*\)\(\(\(unsigned int\)&(\w+)\s*&\s*~3u\)\s*\+\s*4\)")


def patch_source(rel, text, fx):
    """Returns the port's version of one decomp source."""
    t = RE_STATIC_LOCAL.sub(lambda m: ("extern" + m.group(1)) if m.group(2) in config_symbols() else m.group(0), text)
    t = RE_CLZ.sub(r"\1 = (\2) ? (unsigned int)__builtin_clz(\2) : 32u;", t)
    t = RE_REGPIN.sub("", t)
    t = RE_LVALUE_CAST_INC.sub(r"(*(\1 *)((\2 += sizeof(\1)) - sizeof(\1)))", t)
    t = RE_ARRAY_ASSIGN.sub(r"__builtin_memcpy((void *)\2, (const void *)(\3), \1 * sizeof(int));", t)
    # Variadic functions find their trailing arguments the ARM way, one word past the last named
    # parameter's home slot. On wasm the named parameters are locals and the trailing arguments
    # sit in a separate buffer the caller passes, which only the compiler's va_start can name
    # (a wasm32 va_list is a plain pointer into that buffer, read a word at a time like ARM's).
    t = RE_VA_START_MACRO.sub("#define va_start(ap, last) __builtin_va_start(*(__builtin_va_list *)&(ap), last)", t)
    t = RE_VA_INLINE.sub(r"({ __builtin_va_list khvr_ap; __builtin_va_start(khvr_ap, \1); (void *)khvr_ap; })", t)
    # Overlay ids are linker symbols whose *address* is the id. wasm has no absolute symbols, so
    # the id becomes a constant pointer.
    for name, value in SDK_CONSTANTS.items():
        t = re.sub(r"\bextern\s+void\s+%s\s*\(\s*void\s*\)\s*;" % name,
                   "#define %s ((void (*)(void))0x%x)" % (name, value), t)
    if "OVERLAY_" in t:
        t = RE_OVL_DECL.sub("", t)
        t = RE_OVL_ADDR.sub(lambda m: "((unsigned long *)%s)" % (m.group(1) or m.group(2)), t)
        t = RE_OVL.sub(r"((unsigned long *)\1)", t)
    t = ds_layout(t, strip_const=is_data_file(rel))
    t = spin_waits(t)
    for name in fx["unstatic"].get(rel, []):
        # A static helper that the same file (or a header) also declares extern, which mwcc
        # allows: it becomes an ordinary weak definition, and the linker keeps one copy.
        t = re.sub(r"\bstatic\s+(?:inline\s+)?(?=[^;{}()]*\b%s\s*\()" % re.escape(name),
                   "__attribute__((weak)) ", t)
    for name in fx["function_not_data"].get(rel, []):
        # `extern int Fn;` used as `(int)&Fn`: declare it as the function it is.
        t = redeclare(t, name, "extern void %s(void);" % name, function=False)
    for name in fx["data_not_function"].get(rel, []):
        # The other way round: data declared as a function somewhere.
        t = redeclare(t, name, "extern char %s[];" % name, function=True)
    weak = fx["weak"].get(rel, [])
    if weak:
        # Uninitialized globals several files define (mwccarm merges them like -fcommon; wasm has
        # no common symbols): each definition becomes weak and the linker keeps one.
        t = "".join("#pragma weak %s\n" % w for w in weak) + t
    return t


def prepare_sources(srcs, out, fx):
    """Writes patched copies where needed; returns {rel: path to compile}."""
    paths = {}
    for rel in srcs:
        override = GUEST / "overrides" / rel
        src = override if override.exists() else DECOMP / rel
        text = src.read_text(errors="replace")
        patched = patch_source(rel, text, fx)
        if patched == text and not override.exists():
            paths[rel] = DECOMP / rel
            continue
        dst = out / "src" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.exists() or dst.read_text(errors="replace") != patched:
            dst.write_text(patched)
        paths[rel] = dst
    # The guest runtime: port-only C that replaces the libraries' assembly (guest/runtime/).
    for p in sorted((GUEST / "runtime").glob("*.c")):
        paths["guest/runtime/" + p.name] = p
    return paths


# --------------------------------------------------------------------------------------------
# 3. Storage blocks
# --------------------------------------------------------------------------------------------

def parse_module(cfg_dir):
    sections = {}
    for line in (cfg_dir / "delinks.txt").read_text().splitlines():
        if not line.strip():
            break
        m = re.match(r"\s*(\S+)\s+start:0x([0-9a-f]+)\s+end:0x([0-9a-f]+)\s+kind:(\w+)", line)
        if m:
            sections[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    syms = []
    for line in (cfg_dir / "symbols.txt").read_text().splitlines():
        m = re.match(r"(\S+)\s+kind:(\S+)\s+addr:0x([0-9a-f]+)", line)
        if m:
            syms.append((m.group(1), m.group(2), int(m.group(3), 16)))
    return sections, syms


def emit_block(name, start, end, symbols, init=None, section=None):
    """wasm assembly for one contiguous block: `symbols` are (name, addr) inside [start, end),
    `init` an optional list of (addr, directive) for initialized words. Each symbol is sized up
    to the next one, so reads past a symbol land where they did on the DS."""
    section = section or (".data." if init else ".bss.") + name
    lines = ["\t.section\t%s,\"\",@" % section, "\t.globl\t%s" % name, "\t.p2align\t5"]
    addrs = sorted(set(a for _, a in symbols) | set(a for a, _ in (init or [])) | {start})
    by_addr = {}
    for s, a in symbols:
        by_addr.setdefault(a, []).append(s)
    inits = dict(init or [])
    for i, a in enumerate(addrs):
        nxt = addrs[i + 1] if i + 1 < len(addrs) else end
        names = ([name] if a == start else []) + by_addr.get(a, [])
        for s in names:
            if s != name:
                lines.append("\t.globl\t%s" % s)
            lines.append("%s:" % s)
        for s in by_addr.get(a, []):
            lines.append("\t.size\t%s, %d" % (s, nxt - a))
        if a in inits:
            lines.append("\t" + inits[a])
            if nxt - a > 4:
                lines.append("\t.skip\t%d" % (nxt - a - 4))
        elif nxt > a:
            lines.append("\t.skip\t%d" % (nxt - a))
    lines.append("\t.size\t%s, %d" % (name, end - start))
    return "\n".join(lines) + "\n"


TYPE_SIZES = {"u8": 1, "s8": 1, "char": 1, "u16": 2, "s16": 2, "vu16": 2, "short": 2,
              "u64": 8, "s64": 8, "vu64": 8}
RE_SHARED_DECL = re.compile(r"^\s*((?:volatile\s+)?[A-Za-z_][\w ]*?)\s*(\**)\s*([A-Za-z_][\w$]*)\s*(?:\[(\d+)\])?\s*=\s*\{?0\}?\s*;")


def shared_bss_aliases():
    """Addresses of the statics the decomp's `khdays: shared-bss` blocks declare by name.

    Such a block lists a module's zero-initialized statics in the order that makes mwcc lay them
    out like the ROM: reverse declaration order, the last one appended. The members named
    data_XXXXXXXX anchor the block; the others get their addresses from the layout."""
    found, conflicts = {}, set()
    cfg = config_symbols()
    for p in list((DECOMP / "libs").rglob("*.c")) + list((DECOMP / "src").rglob("*.c")):
        text = p.read_text(errors="replace")
        if "khdays: shared-bss" not in text:
            continue
        lines = text.split("khdays: shared-bss", 1)[1].splitlines()[1:]
        decls = []
        for line in lines:
            m = RE_SHARED_DECL.match(line)
            if not m:
                break
            typ, ptr, name, count = m.group(1).strip(), m.group(2), m.group(3), int(m.group(4) or 1)
            base = typ.split()[-1]
            size = 4 if ptr else TYPE_SIZES.get(base, 4 if base[0].isupper() or base in ("int", "BOOL", "u32", "s32", "vu32") else None)
            decls.append((name, size, count))
        if len(decls) < 2:
            continue
        order = list(reversed(decls[:-1])) + [decls[-1]]
        offsets, off, ok = {}, 0, True
        for i, (name, size, count) in enumerate(order):
            if size is None:
                ok = i == len(order) - 1  # an unknown-size type is fine only as the last item
                break
            align = min(size, 4)  # mwcc aligns 64-bit types to 4
            off = (off + align - 1) // align * align
            offsets[name] = off
            off += size * count
        anchors = {n: int(n[5:], 16) - o for n, o in offsets.items() if re.fullmatch(r"data_[0-9a-f]{8}", n)}
        if not ok or not anchors or len(set(anchors.values())) != 1:
            continue
        base = next(iter(anchors.values()))
        for name, o in offsets.items():
            if name in cfg or name in anchors:
                continue
            if name in found and found[name] != base + o:
                conflicts.add(name)
            found[name] = base + o
    return {n: a for n, a in found.items() if n not in conflicts}


def generate_storage(out):
    """Generates <out>/gen/storage.s and the overlay table the platform uses."""
    cfg = DECOMP / "config" / "arm9"
    asm = []
    overlays = {}
    modules = [("main", cfg)] + [(d.name, d) for d in sorted((cfg / "overlays").iterdir()) if d.is_dir()]
    aliases = sorted(shared_bss_aliases().items())
    for line in (GUEST / "aliases.txt").read_text().splitlines():
        line = line.split("#", 1)[0].split()
        if len(line) == 2:
            aliases.append((line[0], int(line[1], 16)))
    (out / "gen").mkdir(parents=True, exist_ok=True)
    (out / "gen" / "aliases.txt").write_text("".join("%s 0x%08x\n" % a for a in aliases))
    for mod, d in modules:
        sections, syms = parse_module(d)
        if ".bss" not in sections:
            continue
        s, e = sections[".bss"]
        bss = [(n, a) for n, k, a in syms if k == "bss" and s <= a < e]
        bss += [(n, a) for n, a in aliases if s <= a < e]
        if e > s:
            asm.append(emit_block("khvr_bss_%s" % mod, s, e, bss))
        if mod != "main":
            overlays[mod] = {"bss": [s, e], "block": "khvr_bss_%s" % mod if e > s else None}

    # The low block (0x027e0000-0x02800000): DTCM with its 0x60 bytes of data (the IRQ handler
    # table and the IRQ thread queue, from libs/nitro/os/auto/os_irq_table_027e0000.c, which
    # exclude.txt drops) and its .bss, then the ARM7-shared page. Named .rodata so wasm-ld lays it
    # out first, at GLOBAL_BASE; wasm memory is writable everywhere regardless of the name.
    low_syms = {}
    for _, d in modules + [("dtcm", cfg / "dtcm"), ("itcm", cfg / "itcm")]:
        for n, k, a in parse_module(d)[1]:
            if GLOBAL_BASE <= a < LOW_END:
                low_syms[n] = a
    table = ["OS_IrqDummy"] * 3 + ["OSi_IrqTimer%d" % i for i in range(4)] + ["OS_IrqDummy"] + \
            ["OSi_IrqDma%d" % i for i in range(4)] + ["OS_IrqDummy"] * 10
    init = [(GLOBAL_BASE + 4 * i, ".int32\t%s" % f) for i, f in enumerate(table)]
    low = list(low_syms.items()) + [("khvr_dtcm", GLOBAL_BASE), ("khvr_shared", 0x027ff000)]
    asm.insert(0, "".join("\t.functype\t%s () -> ()\n" % f for f in sorted(set(table))) +
               emit_block("khvr_low", GLOBAL_BASE, LOW_END, low, init, section=".rodata.khvr_low"))
    gen = out / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    (gen / "storage.s").write_text("\n".join(asm))
    (gen / "overlays.json").write_text(json.dumps(overlays, indent=1))
    return gen / "storage.s"


# --------------------------------------------------------------------------------------------
# 4. Compile and link
# --------------------------------------------------------------------------------------------

def obj_name(rel):
    return rel.replace("/", "_") + ".o"


RE_ASM_DATA_BLOCK = re.compile(r"^\t\.section\t\.data\.([A-Za-z_]\w*),")


def reorder_data_sections(asm):
    """clang emits a file's globals in its own order (a table's initializer pulls the symbols it
    points at ahead of it); this puts the file's per-symbol data sections back in DS address
    order, each block moved whole into the slots the data blocks occupied."""
    addrs = ds_data_addresses()
    lines = asm.split("\n")
    starts = [i for i, l in enumerate(lines) if l.startswith("\t.section\t")]
    blocks = []
    for k, i in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(lines)
        blocks.append((i, end))
    head = lines[:starts[0]] if starts else lines
    chunks = [lines[a:b] for a, b in blocks]
    data = [k for k, (a, _) in enumerate(blocks)
            if RE_ASM_DATA_BLOCK.match(lines[a]) and RE_ASM_DATA_BLOCK.match(lines[a]).group(1) in addrs
            and any(l.startswith(RE_ASM_DATA_BLOCK.match(lines[a]).group(1) + ":") for l in chunks[k])]
    order = sorted(data, key=lambda k: addrs[RE_ASM_DATA_BLOCK.match(chunks[k][0]).group(1)][1])
    out = list(chunks)
    for slot, k in zip(data, order):
        out[slot] = chunks[k]
    return "\n".join(head + [l for c in out for l in c])


def compile_one(rel, path, objdir):
    obj = objdir / obj_name(rel)
    data = Path(path).read_bytes()
    reorder = is_data_file(rel) or b"__attribute__((aligned(" in data
    key = hashlib.sha1(data + " ".join(CFLAGS).encode() + (b" ds-order-2" if reorder else b"")).hexdigest()
    stamp = obj.with_suffix(".key")
    if obj.exists() and stamp.exists() and stamp.read_text() == key:
        return rel, True, ""
    inc = ["-I", str(DECOMP / "include"), "-I", str(GUEST / "include")]
    if reorder:
        # Through assembly, so the data sections can be put in DS order (reorder_data_sections).
        asm = obj.with_suffix(".s")
        r = run(["clang", *CFLAGS, *inc, "-S", str(path), "-o", str(asm)])
        if r.returncode == 0:
            asm.write_text(reorder_data_sections(asm.read_text()))
            r2 = run(["clang", "--target=wasm32", "-c", str(asm), "-o", str(obj)])
            asm.unlink()
            if r2.returncode == 0:
                stamp.write_text(key)
                return rel, True, ""
    r = run(["clang", *CFLAGS, *inc, "-c", str(path), "-o", str(obj)])
    if r.returncode == 0:
        stamp.write_text(key)
        return rel, True, ""
    return rel, False, r.stderr


def compile_all(paths, objdir, jobs):
    objdir.mkdir(parents=True, exist_ok=True)
    failures = {}
    with cf.ThreadPoolExecutor(jobs) as ex:
        for rel, ok, err in ex.map(lambda kv: compile_one(kv[0], kv[1], objdir), paths.items()):
            if not ok:
                failures[rel] = err
    return failures


def link(objs, out):
    wasm = out / "game.wasm"
    rsp = out / "link.rsp"
    rsp.write_text("\n".join(str(o) for o in objs))
    r = run(["wasm-ld", "--no-entry", "--allow-undefined", "--export-all", "--no-gc-sections",
             "--error-limit=0", "--global-base=%d" % GLOBAL_BASE,
             "--initial-memory=%d" % MEMORY_BYTES, "--max-memory=%d" % MEMORY_BYTES,
             "-z", "stack-size=%d" % GUEST_STACK_BYTES, "--table-base=1",
             "--Map=%s" % (out / "game.map"), "-o", str(wasm), "@%s" % rsp])
    (out / "link.log").write_text(r.stdout + r.stderr)
    return r.returncode == 0, r.stdout + r.stderr


def definers_first(log, objs, out):
    """wasm-ld (18) leaves a function out of the function table, so that every address taken of
    it is 0, when an object declaring it with another signature comes before the object that
    defines it (the decomp declares many functions loosely). The objects that define such
    functions, when they hold no data (so the data layout does not move), go right after
    storage.o; returns the new object list, or None when nothing moves."""
    names = set(re.findall(r"function signature mismatch: (\S+)", log))
    if not names:
        return None
    definer, with_data, in_data = {}, set(), False
    for line in (out / "game.map").read_text().splitlines():
        cols = line.split()
        if len(cols) >= 4 and cols[3] == "DATA":
            in_data = True
            continue
        m = re.search(r"(\S+\.o):\(([^)]+)\)$", line)
        if not m:
            continue
        if in_data:
            with_data.add(m.group(1))
        elif m.group(2) in names:
            definer[m.group(2)] = m.group(1)
    moving = {o for o in definer.values() if o not in with_data}
    if not moving:
        return None
    front = [o for o in objs[1:] if str(o) in moving]
    rest = [o for o in objs[1:] if str(o) not in moving]
    print("link: %d objects that define loosely declared functions go first" % len(front))
    return [objs[0]] + front + rest


def obj_to_rel(obj, rels):
    name = Path(obj).name
    for rel in rels:
        if obj_name(rel) == name:
            return rel
    return None


def discover_from_compile(failures, fx):
    found = 0
    for rel, err in failures.items():
        for name in re.findall(r"static declaration of '(\w+)' follows non-static", err):
            if name not in fx["unstatic"].setdefault(rel, []):
                fx["unstatic"][rel].append(name)
                found += 1
    return found


def defines(obj, sym):
    nm = shutil.which("llvm-nm") or shutil.which("llvm-nm-18")
    r = run([nm, "--defined-only", "-j", str(obj)])
    return sym in r.stdout.split()


def discover_from_link(log, rels, fx):
    found = 0
    by_obj = {obj_name(r): r for r in rels}
    blocks = re.split(r"\n(?=wasm-ld: )", log)
    for b in blocks:
        m = re.match(r"wasm-ld: error: symbol type mismatch: (\S+)", b)
        if m:
            sym = m.group(1)
            sides = re.findall(r"defined as WASM_SYMBOL_TYPE_(\w+) in (\S+)", b)
            definer = [kind for kind, obj in sides if defines(obj, sym)]
            if not definer:
                continue
            for kind, obj in sides:
                rel = by_obj.get(Path(obj).name)
                if not rel or kind == definer[0]:
                    continue
                key = "function_not_data" if definer[0] == "FUNCTION" else "data_not_function"
                if sym not in fx[key].setdefault(rel, []):
                    fx[key][rel].append(sym)
                    found += 1
        m = re.match(r"wasm-ld: error: duplicate symbol: (\S+)", b)
        if m:
            for obj in re.findall(r">>> defined in (\S+)", b):
                rel = by_obj.get(Path(obj).name)
                if rel and m.group(1) not in fx["weak"].setdefault(rel, []):
                    fx["weak"][rel].append(m.group(1))
                    found += 1
    return found


def write_overlay_table(out, dst):
    """C table of every overlay's writable data (its .data and .bss input sections, plus its
    generated .bss block), which the platform resets when the game loads the overlay again, as
    loading it from the ROM did."""
    ranges = {}
    blocks = json.loads((out / "gen" / "overlays.json").read_text())
    for line in (out / "game.map").read_text().splitlines():
        m = re.match(r"\s*([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+(\S+):\((\.(?:data|bss)\S*)\)$", line)
        if not m:
            continue
        addr, size, obj, sect = int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4)
        ov = re.search(r"_ov(\d{3})(?:_|\.)", Path(obj).name)
        if sect.startswith(".bss.khvr_bss_ov"):
            ov = re.match(r"\.bss\.khvr_bss_ov(\d{3})", sect)
        if not ov or size == 0:
            continue
        zero = sect.startswith(".bss")
        ranges.setdefault(int(ov.group(1)), []).append([addr, size, zero])
    lines = ["/* Generated by guest/build_guest.py: writable data of each overlay (id, address, size,",
             " * zero-filled). */", "#include <stdint.h>",
             "struct khvr_overlay_range { uint16_t id; uint8_t zero; uint32_t addr, size; };",
             "const struct khvr_overlay_range khvr_overlay_ranges[] = {"]
    count = 0
    for ov in sorted(ranges):
        merged = []
        for a, s, z in sorted(ranges[ov]):
            if merged and merged[-1][2] == z and merged[-1][0] + merged[-1][1] >= a:
                merged[-1][1] = max(merged[-1][1], a + s - merged[-1][0])
            else:
                merged.append([a, s, z])
        for a, s, z in merged:
            lines.append("  {%d, %d, 0x%08x, 0x%x}," % (ov, 1 if z else 0, a, s))
            count += 1
    lines += ["};", "const unsigned khvr_overlay_range_count = %d;" % count]
    (dst / "khvr_overlays.c").write_text("\n".join(lines) + "\n")
    return len(ranges), count


def undefined_data(log):
    return sorted(set(re.findall(r"wasm-ld: error: .*undefined symbol: (\S+)", log)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "build" / "guest"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--discover", action="store_true", help="find and record new fixups")
    ap.add_argument("--no-wasm2c", action="store_true", help="stop after linking")
    args = ap.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    for tool in ("clang", "wasm-ld", "wasm2c"):
        if not shutil.which(tool):
            sys.exit("missing tool: %s (clang/lld 17+ and wabt 1.0.34+)" % tool)

    fx = load_fixups()
    srcs = collect_sources()
    storage = generate_storage(out)
    placeholders = set()
    for attempt in range(8):
        paths = prepare_sources(srcs, out, fx)
        failures = compile_all(paths, out / "obj", args.jobs)
        ok = False
        new = discover_from_compile(failures, fx) if args.discover else 0
        if new:
            print("compile: %d new fixups, retrying" % new)
            continue
        r = run(["clang", "--target=wasm32", "-c", str(storage), "-o", str(out / "obj" / "storage.o")])
        if r.returncode:
            sys.exit("storage.s: " + r.stderr)
        # storage.o first: its low block must be the first data wasm-ld lays out.
        # Data files first, in DS order (ds_layout keeps each one's symbols in DS order).
        built = [rel for rel in paths if rel not in failures]
        keys = {rel: data_file_order(rel, paths[rel]) for rel in built}
        built.sort(key=lambda rel: (0, keys[rel]) if keys[rel] is not None else (1, ("", 0)))
        objs = [out / "obj" / "storage.o"] + [out / "obj" / obj_name(rel) for rel in built]
        if placeholders:
            ph = out / "gen" / "placeholders.s"
            ph.write_text("".join(emit_block("khvr_ph_" + s, 0, 64, [(s, 0)]) for s in sorted(placeholders)))
            r = run(["clang", "--target=wasm32", "-c", str(ph), "-o", str(out / "obj" / "placeholders.o")])
            if r.returncode:
                sys.exit("placeholders.s: " + r.stderr)
            objs.append(out / "obj" / "placeholders.o")
        ok, log = link(objs, out)
        if ok:
            reordered = definers_first(log, objs, out)
            if reordered:
                objs = reordered
                ok, log = link(objs, out)
        missing = set(undefined_data(log)) - placeholders
        if missing:
            # Data nobody defines (left out with a failed or excluded source): 64 zero bytes each,
            # listed in placeholders.txt so they can be given real storage.
            placeholders |= missing
            print("link: %d undefined data symbols get placeholders, retrying" % len(missing))
            continue
        new = discover_from_link(log, list(paths), fx) if args.discover else 0
        if new:
            print("link: %d new fixups, retrying" % new)
            continue
        break
    if args.discover:
        save_fixups(fx)

    (out / "placeholders.txt").write_text("".join(s + "\n" for s in sorted(placeholders)))
    (out / "compile_failures.txt").write_text(
        "".join("== %s\n%s\n" % (rel, err) for rel, err in sorted(failures.items())))
    print("compiled %d / %d sources (%d failed, see compile_failures.txt)"
          % (len(paths) - len(failures), len(paths), len(failures)))
    if not ok:
        errors = [l for l in log.splitlines() if l.startswith("wasm-ld: error")]
        print("link failed with %d errors, first ones:" % len(errors))
        print("\n".join(errors[:20]))
        sys.exit(1)
    sigs = len(re.findall(r"function signature mismatch", log))
    print("linked %s (%d KB), %d signature mismatches adapted at runtime"
          % (out / "game.wasm", (out / "game.wasm").stat().st_size // 1024, sigs))
    if args.no_wasm2c:
        return
    (out / "wasm2c").mkdir(exist_ok=True)
    r = run(["wasm2c", "--num-outputs=16", "--module-name=khdays", str(out / "game.wasm"),
             "-o", str(out / "wasm2c" / "khdays.c")])
    if r.returncode:
        sys.exit("wasm2c: " + r.stderr)
    r = run([sys.executable, str(GUEST / "postprocess.py"), str(out / "wasm2c"), str(out / "link.log")])
    sys.stdout.write(r.stdout)
    if r.returncode:
        sys.exit("postprocess: " + r.stderr)
    n, count = write_overlay_table(out, out / "wasm2c")
    print("overlay table: %d overlays, %d data ranges" % (n, count))


if __name__ == "__main__":
    main()
