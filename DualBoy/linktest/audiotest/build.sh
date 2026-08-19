#!/usr/bin/env bash
# Build the DualBoy GBA audio test ROM (square wave on channel 2).
set -euo pipefail
cd "$(dirname "$0")"
CLANG=${CLANG:-clang}
"$CLANG" --target=arm-none-eabi -mcpu=arm7tdmi -marm -O2 \
    -ffreestanding -fno-builtin -Wall -Wextra -c crt0.s -o crt0.o
"$CLANG" --target=arm-none-eabi -mcpu=arm7tdmi -marm -O2 \
    -ffreestanding -fno-builtin -Wall -Wextra -c main.c -o main.o
arm-none-eabi-ld -T link.ld -o audiotest.elf crt0.o main.o
arm-none-eabi-objcopy -O binary audiotest.elf audiotest.gba
python3 fix_header.py audiotest.gba
echo "built: $(pwd)/audiotest.gba ($(stat -c%s audiotest.gba) bytes)"
