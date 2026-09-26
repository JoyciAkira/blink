#!/bin/bash
set -euo pipefail

BLINK_SRC="/Volumes/External/DANI/BLINK-VEP-V1"
MUSL="/Volumes/External/DANI/VEP-TOOLCHAIN/musl-install/usr/local/musl"
COMPILER_RT="/Volumes/External/DANI/VEP-TOOLCHAIN/compiler-rt-wasm/libcompiler_rt_wasm.a"
CLANG="/opt/homebrew/opt/llvm/bin/clang"
OUTDIR="/tmp/blink-build-instrumented"
OUTPUT="/Volumes/External/DANI/BLINK-VEP-V1/experiment/blink-instrumented.wasm"

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

CFLAGS="--target=wasm32 -matomics -mbulk-memory -O2 -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
CFLAGS="$CFLAGS -isystem $MUSL/include"
CFLAGS="$CFLAGS -isystem $BLINK_SRC/tool/stdatomic"
CFLAGS="$CFLAGS -I$BLINK_SRC"
CFLAGS="$CFLAGS -include $BLINK_SRC/blink-mman-shim.h"
CFLAGS="$CFLAGS -Wno-macro-redefined -Wno-unused-command-line-argument"

cd "$BLINK_SRC"

echo "=== Compiling Blink C files ==="
SRC_FILES=()
for f in blink/*.c; do
    basename=$(basename "$f")
    case "$basename" in
        demangle.c|blinkenlights.c|oneoff.c|compress.c)
            continue
            ;;
    esac
    SRC_FILES+=("$f")
done

for src in "${SRC_FILES[@]}"; do
    $CLANG $CFLAGS -c -o "$OUTDIR/$(basename "$src" .c).o" "$src" &
done
wait

echo "=== Compiling shims ==="
$CLANG $CFLAGS -c -o "$OUTDIR/blink-wasm-mman-impl.o" "blink-wasm-mman-impl.c"
$CLANG $CFLAGS -c -o "$OUTDIR/wasm-sjlj-runtime.o" "wasm-sjlj-runtime.c"

echo "=== Linking blink-instrumented.wasm ==="
$CLANG --target=wasm32 \
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
    -o "$OUTPUT"

echo "=== Build Complete ==="
sha256sum "$OUTPUT"
ls -lh "$OUTPUT"
