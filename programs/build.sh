#!/bin/bash
# Build WAMR AOT programs for the ESP32 (Xtensa) and put them into ../fs_image/bin.
#   ./build.sh            build all *.c
#   ./build.sh hello.c    build one
# Needs: wasi-sdk (WASI_SDK env or ~/esp/tools/wasi/wasi-sdk-*) and an Xtensa-capable wamrc
# (WAMRC env or ~/esp/wamr/wamr-compiler/build/wamrc). See ../README.md.
set -euo pipefail
cd "$(dirname "$0")"

WASI_SDK=${WASI_SDK:-$(ls -d "$HOME"/esp/tools/wasi/wasi-sdk-* 2>/dev/null | grep -v tar.gz | head -1)}
WAMRC=${WAMRC:-$HOME/esp/wamr/wamr-compiler/build/wamrc}
OUT=${OUT:-../fs_image/bin}
WORK=${TMPDIR:-/tmp}/esp32-unix-build
mkdir -p "$OUT" "$WORK"
[ -x "$WASI_SDK/bin/clang" ] || { echo "wasi-sdk not found (set WASI_SDK)"; exit 1; }
[ -x "$WAMRC" ] || { echo "wamrc not found (set WAMRC)"; exit 1; }

SRCS=("$@"); [ ${#SRCS[@]} -gt 0 ] || SRCS=(*.c)
for src in "${SRCS[@]}"; do
    name=${src%.c}
    # Freestanding build (no wasi-libc): programs are a few KB instead of 30+ KB, which matters because AOT
    # code and the file are held in the ESP32's scarce RAM. Linear memory: 64 KiB (1 page); 8 KiB stack.
    "$WASI_SDK/bin/clang" --target=wasm32 -nostdlib -ffreestanding -fno-builtin \
        -Oz -z stack-size=8192 -Wl,--no-entry -Wl,--export=esp_main \
        -Wl,--gc-sections -Wl,--strip-all \
        -Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
        -o "$WORK/$name.wasm" "$src"
    "$WAMRC" ${WAMRC_FLAGS:---target=xtensa --opt-level=2 --size-level=0} -o "$OUT/$name.aot" "$WORK/$name.wasm"
    printf '%-8s wasm %6s B  ->  aot %6s B\n' "$name" "$(stat -c%s "$WORK/$name.wasm")" "$(stat -c%s "$OUT/$name.aot")"
done
