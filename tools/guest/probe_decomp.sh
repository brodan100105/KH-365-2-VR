#!/usr/bin/env bash
# Measures how far the decompiled game is from building as the port's 32-bit "guest" module
# (docs/ARCHITECTURE.md): compiles every C file of decomp/ for wasm32 (an ILP32 target, so
# pointers and `int`s stay 32 bits as on the DS), then links them and counts what the linker
# rejects. Needs clang and wasm-ld (LLVM 17 or newer).
#
#   tools/guest/probe_decomp.sh [decomp dir] [work dir]
#
# Prints a summary; the full lists are in the work dir (compile_failures.txt, link.log).
set -euo pipefail

DECOMP=$(realpath "${1:-decomp}")
WORK=$(realpath -m "${2:-build/guest-probe}")
JOBS=${JOBS:-$(nproc)}
mkdir -p "$WORK/obj"

# The decomp is written for mwccarm, which accepts what modern clang turns into errors by
# default; these keep them warnings so the probe measures real incompatibilities.
CFLAGS=(--target=wasm32 -ffreestanding -nostdinc -fno-builtin -O1 -w
        -I"$DECOMP/include"
        -Wno-error=int-conversion -Wno-error=implicit-function-declaration
        -Wno-error=return-type -Wno-error=incompatible-function-pointer-types
        -Wno-error=implicit-int)

cd "$DECOMP"
find src libs -name '*.c' | sort > "$WORK/sources.txt"
export WORK
printf '%s\0' "${CFLAGS[@]}" > "$WORK/cflags"
compile_one() {
    mapfile -d '' flags < "$WORK/cflags"
    obj="$WORK/obj/$(echo "$1" | tr / _).o"
    clang "${flags[@]}" -c "$1" -o "$obj" 2>/dev/null || echo "$1"
}
export -f compile_one
xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {} < "$WORK/sources.txt" > "$WORK/compile_failures.txt" || true

total=$(wc -l < "$WORK/sources.txt")
failed=$(wc -l < "$WORK/compile_failures.txt")
game_total=$(grep -c '^src/' "$WORK/sources.txt" || true)
game_failed=$(grep -c '^src/' "$WORK/compile_failures.txt" || true)

cd "$WORK/obj"
wasm-ld --no-entry --allow-undefined --export-all --no-gc-sections --error-limit=0 \
    -o "$WORK/game.wasm" ./*.o > "$WORK/link.log" 2>&1 || true

dup=$(grep 'duplicate symbol' "$WORK/link.log" | awk '{print $NF}' | sort -u | wc -l)
mismatch=$(grep 'symbol type mismatch' "$WORK/link.log" | awk '{print $NF}' | sort -u | wc -l)
sig=$(grep -c 'function signature mismatch' "$WORK/link.log" || true)

cat <<EOF
decomp:                       $DECOMP ($(git -C "$DECOMP" rev-parse --short HEAD 2>/dev/null || echo '?'))
C files compiled for wasm32:  $((total - failed)) / $total
  game code (src/):           $((game_total - game_failed)) / $game_total
  libraries (libs/):          $((total - game_total - failed + game_failed)) / $((total - game_total))
link: duplicate definitions:  $dup symbols   (uninitialized globals defined in several files)
link: function/data clashes:  $mismatch symbols   (a function declared as data elsewhere)
link: signature mismatches:   $sig   (only reported once the errors above are fixed)
details: $WORK/compile_failures.txt, $WORK/link.log
EOF
