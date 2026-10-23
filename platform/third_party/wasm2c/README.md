Vendored from WABT 1.0.34 (wasm2c/wasm-rt*.{h,c}), Apache License 2.0.
https://github.com/WebAssembly/wabt

Local changes, both so wasm-rt.h also compiles as C++: it initializes wasm_rt_funcref_null_value,
and WASM_RT_THREAD_LOCAL is `thread_local` under C++.

wasm-rt-impl.c calls a weak `khvr_on_trap(code)` before unwinding, so the platform can log where
the guest trapped.
