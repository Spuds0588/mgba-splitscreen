#!/usr/bin/env bash
# Build the DualBoy GBA linktest ROM.
#
# Needs: clang (ARM target) + binutils-arm-none-eabi
#   (arm-none-eabi-ld / arm-none-eabi-objcopy). No libc, no devkitARM:
#   crt0.s is a hand-written boot header + entry, main.c is freestanding C.
set -euo pipefail
cd "$(dirname "$0")"

CLANG=${CLANG:-clang}

"$CLANG" --target=arm-none-eabi -mcpu=arm7tdmi -marm -O2 \
    -ffreestanding -fno-builtin -Wall -Wextra -c crt0.s -o crt0.o
"$CLANG" --target=arm-none-eabi -mcpu=arm7tdmi -marm -O2 \
    -ffreestanding -fno-builtin -Wall -Wextra -c main.c -o main.o
arm-none-eabi-ld -T link.ld -o linktest.elf crt0.o main.o
arm-none-eabi-objcopy -O binary linktest.elf linktest.gba
python3 fix_header.py linktest.gba
echo "built: $(pwd)/linktest.gba ($(stat -c%s linktest.gba) bytes)"
