#!/bin/bash
# build-wasm.sh - Compile blink for wasm32 with fixed mmap shim
# Uses __builtin_wasm_memory_grow instead of posix_memalign

set -euo pipefail

BLINK_SRC="/tmp/blink-d1f4"
MUSL="$HOME/tombl-build/musl-install/usr/local/musl"
COMPILER_RT="$HOME/tombl-build/compiler-rt-wasm/libcompiler_rt_wasm.a"
OUTDIR="/tmp/blink-wasm-build"
OUTPUT="$BLINK_SRC/blink-fixed.wasm"

# Clean and create build dir
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

CFLAGS="--target=wasm32 -matomics -mbulk-memory -O2 -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
CFLAGS="$CFLAGS -isystem $MUSL/include"
CFLAGS="$CFLAGS -isystem $BLINK_SRC/tool/stdatomic"
CFLAGS="$CFLAGS -I$BLINK_SRC"
CFLAGS="$CFLAGS -include $BLINK_SRC/blink-mman-shim.h"
CFLAGS="$CFLAGS -Wno-macro-redefined -Wno-unused-command-line-argument"

cd "$BLINK_SRC"

echo "=== Compiling blink source files ==="
SRC_FILES=()
for f in blink/*.c; do
    basename=$(basename "$f")
    # Skip: demangle.c (our shim provides Demangle),
    #       blinkenlights.c (TUI binary, not headless blink),
    #       oneoff.c (test binary),
    #       compress.c (needs zlib, only used by blinkenlights)
    case "$basename" in
        demangle.c|blinkenlights.c|oneoff.c|compress.c)
            echo "  SKIP $f" >&2
            continue
            ;;
    esac
    SRC_FILES+=("$f")
done

echo "Total source files: ${#SRC_FILES[@]}"

# Compile all source files (parallel)
for src in "${SRC_FILES[@]}"; do
    echo "  CC  $src" >&2
    clang $CFLAGS -c -o "$OUTDIR/$(basename "$src" .c).o" "$src" &
done
wait
echo "All source files compiled."

# Compile the fixed mmap shim
echo "  CC  blink-wasm-mman-impl.c"
clang $CFLAGS -c -o "$OUTDIR/blink-wasm-mman-impl.o" "blink-wasm-mman-impl.c" 2>&1
clang $CFLAGS -c -o "$OUTDIR/wasm-sjlj-runtime.o" "wasm-sjlj-runtime.c" 2>&1

# Count
OBJ_COUNT=$(ls "$OUTDIR"/*.o 2>/dev/null | wc -l)
echo ""
echo "=== Linking blink.wasm ($OBJ_COUNT objects) ==="

clang --target=wasm32 \
    -mllvm -wasm-enable-sjlj \
    -mllvm -wasm-use-legacy-eh=false \
    -nostartfiles -nostdlib \
    "$OUTDIR"/*.o \
    "$MUSL/lib/crt1.o" \
    -L"$MUSL/lib" -lc \
    "$COMPILER_RT" \
    -Wl,--entry=_start \
    -Wl,--export=_start \
    -Wl,--export=__indirect_function_table \
    -Wl,--shared-memory \
    -Wl,--initial-memory=2097152 \
    -Wl,--max-memory=4294967296 \
    -Wl,--import-memory \
    -Wl,--gc-sections \
    -o "$OUTPUT" 2>&1

echo ""
echo "=== Build complete ==="
echo "Output: $OUTPUT"
ls -lh "$OUTPUT"

echo ""
echo "=== Checking memory import ==="
wasm-objdump -x "$OUTPUT" 2>/dev/null | grep -E "memory|env\."

echo ""
echo "=== Checking exports ==="
wasm-objdump -x "$OUTPUT" 2>/dev/null | grep -E "Export\[|_start|__indirect"
