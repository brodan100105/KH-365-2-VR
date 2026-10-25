#!/usr/bin/env python3
"""Adapts wasm2c's output of the guest for the port.

    guest/postprocess.py <wasm2c dir> <link.log>

1. I/O and VRAM: every load and store whose address lies in 0x04xxxxxx (I/O registers) or
   0x06xxxxxx (VRAM, which the DS maps bank by bank) goes to the platform's khvr_io_read /
   khvr_io_write. Everything else stays a plain memory access.
2. Indirect calls: the decomp calls through function pointers declared with other signatures
   than the callee's (as C on ARM allows), so the wasm type check is dropped; the call goes
   through the pointer like the original's `blx`.
3. Signature mismatches: wasm-ld replaces direct calls whose declaration disagrees with the
   definition by a stub that traps. The stubs become adapters that call the real function,
   dropping extra arguments, passing 0 for missing ones and converting the return value: the
   same thing the ARM calling convention did in the original.
4. The stack pointer global becomes thread-local, because every guest OS thread runs on its own
   host thread with its own stack (platform/src/hle_thread.cpp).
5. Imports the platform does not implement get weak stubs that log their first call.
"""
import re
import sys
from pathlib import Path

PRELUDE_LOAD = """#define DEFINE_LOAD(name, t1, t2, t3, force_read)          \\
  static inline t3 name(wasm_rt_memory_t* mem, u64 addr) { \\
    MEMCHECK(mem, addr, t1);                               \\
    t1 result;                                             \\
    wasm_rt_memcpy(&result, &mem->data[addr], sizeof(t1)); \\
    force_read(result);                                    \\
    return (t3)(t2)result;                                 \\
  }"""

PRELUDE_STORE = """#define DEFINE_STORE(name, t1, t2)                                     \\
  static inline void name(wasm_rt_memory_t* mem, u64 addr, t2 value) { \\
    MEMCHECK(mem, addr, t1);                                           \\
    t1 wrapped = (t1)value;                                            \\
    wasm_rt_memcpy(&mem->data[addr], &wrapped, sizeof(t1));            \\
  }"""

IO_LOAD = """/* khvr: I/O (0x04xxxxxx) and VRAM (0x06xxxxxx) accesses go to the platform layer. */
#define KHVR_IS_IO(addr) ((((u32)((addr) >> 24)) | 2u) == 6u)
u64 khvr_io_read(u32 addr, u32 size);
void khvr_io_write(u32 addr, u64 value, u32 size);
#define DEFINE_LOAD(name, t1, t2, t3, force_read)                         \\
  static inline t3 name(wasm_rt_memory_t* mem, u64 addr) {                \\
    if (UNLIKELY(KHVR_IS_IO(addr)))                                       \\
      return (t3)(t2)(t1)khvr_io_read((u32)addr, sizeof(t1));             \\
    MEMCHECK(mem, addr, t1);                                              \\
    t1 result;                                                            \\
    wasm_rt_memcpy(&result, &mem->data[addr], sizeof(t1));                \\
    force_read(result);                                                   \\
    return (t3)(t2)result;                                                \\
  }"""

IO_STORE = """#define DEFINE_STORE(name, t1, t2)                                        \\
  static inline void name(wasm_rt_memory_t* mem, u64 addr, t2 value) {    \\
    t1 wrapped = (t1)value;                                               \\
    if (UNLIKELY(KHVR_IS_IO(addr))) {                                     \\
      u64 bits = 0;                                                       \\
      wasm_rt_memcpy(&bits, &wrapped, sizeof(t1));                        \\
      khvr_io_write((u32)addr, bits, sizeof(t1));                         \\
      return;                                                             \\
    }                                                                     \\
    MEMCHECK(mem, addr, t1);                                              \\
    wasm_rt_memcpy(&mem->data[addr], &wrapped, sizeof(t1));               \\
  }"""

CHECK_INDIRECT = """#define CHECK_CALL_INDIRECT(table, ft, x)                \\
  (LIKELY((x) < table.size && table.data[x].func &&      \\
          func_types_eq(ft, table.data[x].func_type)) || \\
   TRAP(CALL_INDIRECT))"""

LENIENT_INDIRECT = """/* khvr: no signature check; mismatched calls behave as on ARM (extra arguments ignored). */
#define CHECK_CALL_INDIRECT(table, ft, x)           \\
  (LIKELY((x) < table.size && table.data[x].func) || \\
   TRAP(CALL_INDIRECT))"""

SP_FIELD = "instance->w2c_0x5F_stack_pointer"
SP_TLS = "khvr_guest_sp"

RE_PROTO = re.compile(r"^(u32|u64|f32|f64|void) (w2c_\w+)\(([^)]*)\);", re.M)
RE_STUB = re.compile(r"^(u32|u64|f32|f64|void) (w2c_khdays_signature_mismatch0x3A(\w+))\(([^)]*)\) \{\n(.*?)\n\}",
                     re.M | re.S)


def params(text):
    """Parameter types of a wasm2c prototype or definition, without the instance pointer."""
    out = []
    for p in [x.strip() for x in text.split(",")][1:]:
        out.append(p.split()[0])
    return out


def convert(expr, src, dst):
    if src == dst:
        return expr
    return "(%s)(%s)" % (dst, expr)


def adapter(ret, name, stub_params, target, target_ret, target_params, env):
    args = []
    for i, t in enumerate(target_params):
        args.append(convert("var_p%d" % i, stub_params[i], t) if i < len(stub_params) else "0")
    ctx = "instance->w2c_env_instance" if env else "instance"
    call = "%s(%s%s)" % (target, ctx, "".join(", " + a for a in args))
    params_decl = "".join(", %s var_p%d" % (t, i) for i, t in enumerate(stub_params))
    head = "%s %s(w2c_khdays* instance%s) {\n" % (ret, name, params_decl)
    if ret == "void":
        body = "  %s;\n" % call
    elif target_ret == "void":
        body = "  %s;\n  return 0;\n" % call
    else:
        body = "  return %s;\n" % convert(call, target_ret, ret)
    return head + "  /* khvr: adapter for a call declared with another signature */\n" + body + "}"


def main():
    d = Path(sys.argv[1])
    impl = d / "khdays-impl.h"
    text = impl.read_text()
    for old, new in ((PRELUDE_LOAD, IO_LOAD), (PRELUDE_STORE, IO_STORE), (CHECK_INDIRECT, LENIENT_INDIRECT)):
        if old not in text:
            sys.exit("postprocess: wasm2c prelude changed, update guest/postprocess.py (%s)" % old.split("(")[0])
        text = text.replace(old, new, 1)
    text = text.replace('#include "khdays.h"',
                        '#include "khdays.h"\n/* khvr: each guest thread has its own stack. */\n'
                        'extern _Thread_local u32 khvr_guest_sp;', 1)
    impl.write_text(text.replace(SP_FIELD, SP_TLS))

    header = (d / "khdays.h").read_text()
    protos = {m.group(2): (m.group(1), params(m.group(3))) for m in RE_PROTO.finditer(header)}
    imports = {n: v for n, v in protos.items() if n.startswith("w2c_env_")}

    adapted = unresolved = 0
    for c in sorted(d.glob("khdays_*.c")):
        src = c.read_text().replace(SP_FIELD, SP_TLS)

        def fix(m):
            nonlocal adapted, unresolved
            ret, name, target_name, plist, body = m.groups()
            if "UNREACHABLE" not in body:
                return m.group(0)
            stub_params = [p.strip().split()[0] for p in plist.split(",")[1:]]
            # wasm-ld names the second, third... distinct stubs of one function `Name.1`, `Name.2`.
            target_name = re.sub(r"0x2E\d+$", "", target_name)
            for cand, env in (("w2c_khdays_" + target_name, False), ("w2c_env_" + target_name, True)):
                if cand in protos:
                    tret, tparams = protos[cand]
                    adapted += 1
                    return adapter(ret, name, stub_params, cand, tret, tparams, env)
            unresolved += 1
            return m.group(0)

        src = RE_STUB.sub(fix, src)
        c.write_text(src)

    # Weak logging stubs for every import, so the guest links before the platform implements
    # them; the platform's own (strong) definitions win.
    out = ['/* Generated by guest/postprocess.py: weak fallbacks for the guest\'s imports. */',
           '#include "khdays.h"', 'void khvr_unimplemented_import(const char *name);', '']
    for name, (ret, ps) in sorted(imports.items()):
        decl = "".join(", %s p%d" % (t, i) for i, t in enumerate(ps))
        out.append("__attribute__((weak)) %s %s(struct w2c_env* env%s) {" % (ret, name, decl))
        out.append('  khvr_unimplemented_import("%s");' % name[len("w2c_env_"):])
        if ret != "void":
            out.append("  return 0;")
        out.append("}")
    (d / "khdays_imports.c").write_text("\n".join(out) + "\n")

    # Exported data symbols by name, for the platform's SymbolAddress (wasm2c's accessors return
    # a pointer to the u32 global holding the symbol's address). Names wasm2c had to mangle are
    # left out; the platform only looks up plain C identifiers.
    syms = sorted(re.findall(r"^u32\* w2c_khdays_([A-Za-z_]\w*)\(w2c_khdays\* instance\);", header, re.M))
    syms = [s for s in syms if "0x" not in s]
    tab = ['/* Generated by guest/postprocess.py: exported guest symbols by name. */', '#include "khdays.h"',
           'typedef u32 *(*khvr_sym_fn)(w2c_khdays *);',
           'struct khvr_guest_symbol { const char *name; khvr_sym_fn get; };',
           'const struct khvr_guest_symbol khvr_guest_symbols[] = {']
    tab += ['  {"%s", w2c_khdays_%s},' % (s, s) for s in syms]
    tab += ['};', 'const unsigned khvr_guest_symbol_count = %d;' % len(syms)]
    (d / "khdays_symbols.c").write_text("\n".join(tab) + "\n")

    # Function names by table index (a guest function pointer is its index in the indirect
    # function table), for the platform's logs: wasm2c's element segment lists them in order from
    # the destination offset of its funcref_table_init.
    names = []
    for c in sorted(d.glob("khdays_*.c")):
        src = c.read_text()
        seg = re.search(r"elem_segment_exprs_w2c_khdays_e0\[\] = \{(.*?)\n\};", src, re.S)
        init = re.search(r"funcref_table_init\([^,]+, elem_segment_exprs_w2c_khdays_e0, \d+, (\d+)u", src)
        if seg and init:
            offset = int(init.group(1))
            names = [""] * offset
            # One entry per line; a null entry (or anything unnamed) keeps its slot empty.
            for line in seg.group(1).split("\n"):
                if "{Ref" not in line:
                    continue
                m = re.search(r"\(wasm_rt_function_ptr_t\)w2c_(?:khdays|env)_(\w+)", line)
                n = re.sub(r"0x([0-9A-F]{2})", lambda h: chr(int(h.group(1), 16)), m.group(1)) if m else ""
                names.append(re.sub(r"_\d+$", "", n))
            break
    out = ['/* Generated by guest/postprocess.py: guest function names by table index. */',
           'const char *const khvr_function_names[] = {']
    out += ['  "%s",' % n.replace('"', "") for n in names]
    out += ['};', 'const unsigned khvr_function_name_count = %d;' % len(names)]
    (d / "khvr_function_names.c").write_text("\n".join(out) + "\n")
    print("postprocess: %d signature adapters, %d unresolved, %d imports, %d symbols, %d function names"
          % (adapted, unresolved, len(imports), len(syms), len(names)))


if __name__ == "__main__":
    main()
