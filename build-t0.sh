#!/bin/bash
# build-wasm.sh - Compile blink for wasm32 with fixed mmap shim
# Uses __builtin_wasm_memory_grow instead of posix_memalign

set -euo pipefail

BLINK_SRC="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CC="${CC:-/opt/homebrew/opt/llvm/bin/clang}"
SDK_INPUTS="${SDK_INPUTS:-$HOME/zeronode-sdk-rootfs-inputs}"
SDK_INPUTS="$(cd -- "$SDK_INPUTS" && pwd)"
MUSL="$SDK_INPUTS/sysroot"
COMPILER_RT="$SDK_INPUTS/compiler-rt/libcompiler_rt_wasm.a"

for input in "$CC" "$MUSL/include/unistd.h" "$MUSL/lib/crt1.o" \
             "$MUSL/lib/libc.a" "$COMPILER_RT"; do
    if [[ ! -f "$input" ]]; then
        echo "Missing SDK build input: $input" >&2
        exit 1
    fi
done
if [[ -n "${OUTDIR:-}" ]]; then
    mkdir "$OUTDIR"
else
    OUTDIR="$(mktemp -d "${TMPDIR:-/tmp}/blink-t0.XXXXXX")"
fi
OUTDIR="$(cd -- "$OUTDIR" && pwd)"
OUTPUT="$OUTDIR/blink.wasm"
echo "Candidate output directory: $OUTDIR"

# ponytail: retain the qualified __FILE__ prefix; use a portable prefix only
# when qualifying a new binary. Sources still come from this Git worktree.
CFLAGS=(--target=wasm32 -matomics -mbulk-memory -O2
        -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false
        -isystem "$MUSL/include" -isystem "$BLINK_SRC/tool/stdatomic"
        "-I$BLINK_SRC" -include "$BLINK_SRC/blink-mman-shim.h"
        "-fmacro-prefix-map=$BLINK_SRC=/Users/capyco/tombl-build/z1-t0-runtime.yO5TKB/blink-source"
        '-DBUILD_TIMESTAMP="Thu Oct 22 14:17:25 2026"'
        -Wno-macro-redefined
        -Wno-unused-command-line-argument)

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

SRC_FILES+=("blink-wasm-mman-impl.c")
echo "Total source files: ${#SRC_FILES[@]}"

# Compile in the foreground: set -e stops before linking on the first failure.
OBJECTS=()
for src in "${SRC_FILES[@]}"; do
    obj="$OUTDIR/$(basename "$src" .c).o"
    echo "  CC  $src"
    "$CC" "${CFLAGS[@]}" -c -o "$obj" "$src"
    OBJECTS+=("$obj")
done
echo "All ${#OBJECTS[@]} source files compiled."

echo "=== Linking blink.wasm (${#OBJECTS[@]} objects) ==="
"$CC" --target=wasm32 -nostartfiles -nostdlib \
    "${OBJECTS[@]}" "$MUSL/lib/crt1.o" \
    -L"$MUSL/lib" -lc "$COMPILER_RT" \
    -Wl,--entry=_start \
    -Wl,--export=_start \
    -Wl,--export=__indirect_function_table \
    -Wl,--shared-memory \
    -Wl,--initial-memory=2097152 \
    -Wl,--max-memory=4294967296 \
    -Wl,--import-memory \
    -Wl,--gc-sections \
    -o "$OUTPUT"

# Validate the linked module and retain the exact SDK inputs, source flags,
# and ALL header dependencies (config.h, thread.h, shim, tool/stdatomic,
# SDK include tree) so that any header-induced build change is captured.
node - "$OUTDIR" "$CC" "$MUSL" "$COMPILER_RT" "$BLINK_SRC" "${CFLAGS[@]}" -- "${SRC_FILES[@]}" <<'NODE'
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {execFileSync} = require('node:child_process');
const [out, cc, sysroot, compilerRt, blinkSrc, ...args] = process.argv.slice(2);
const separator = args.indexOf('--');
const sha256 = p => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const walkHeaders = (dir) => {
    const results = [];
    for (const entry of fs.readdirSync(dir, {withFileTypes: true})) {
        const full = path.join(dir, entry.name);
        if (entry.isDirectory()) results.push(...walkHeaders(full));
        else if (/\.(h|hpp)$/.test(entry.name)) results.push(full);
    }
    return results;
};
const binary = `${out}/blink.wasm`;
const bytes = fs.readFileSync(binary);
const module = new WebAssembly.Module(bytes);
/* Collect header deps: blink-source headers + tool/stdatomic + config.h + shim + SDK includes */
const headerPaths = [
    ...walkHeaders(path.join(blinkSrc, 'blink')),
    ...walkHeaders(path.join(blinkSrc, 'tool', 'stdatomic')),
    path.join(blinkSrc, 'config.h'),
    path.join(blinkSrc, 'blink-mman-shim.h'),
    ...walkHeaders(path.join(sysroot, 'include'))
].sort();
const receipt = {
    status: 'BUILT_NOT_EXECUTION_CERTIFIED',
    builtAt: new Date().toISOString(),
    clang: execFileSync(cc, ['--version'], {encoding: 'utf8'}),
    flags: args.slice(0, separator),
    sources: args.slice(separator + 1).map(p => ({path: p, sha256: sha256(p)})),
    headers: headerPaths.map(p => ({path: p, sha256: sha256(p)})),
    buildScript: {path: path.join(blinkSrc, 'build-t0.sh'), sha256: sha256(path.join(blinkSrc, 'build-t0.sh'))},
    sdk: [sysroot + '/lib/crt1.o', sysroot + '/lib/libc.a', compilerRt]
        .map(p => ({path: p, sha256: sha256(p)})),
    binary, bytes: bytes.length, sha256: sha256(binary),
    imports: WebAssembly.Module.imports(module),
    exports: WebAssembly.Module.exports(module)
};
fs.writeFileSync(`${out}/build-receipt.json`, JSON.stringify(receipt, null, 2) + '\n');
console.log('BLINK_BUILD_OK', JSON.stringify({
    binary, bytes: receipt.bytes, sha256: receipt.sha256,
    sources: receipt.sources.length, headers: receipt.headers.length, status: receipt.status
}));
NODE
