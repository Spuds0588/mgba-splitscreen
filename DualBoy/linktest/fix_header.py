#!/usr/bin/env python3
"""Post-process a freshly linked .gba: verify the Nintendo logo (mGBA boots
through the BIOS only if the logo CRC matches LOGO_CRC32), set the header
checksum byte at 0xBD, and pad the image to a 0x200 multiple."""
import sys
import zlib

LOGO_CRC32 = 0xD0BEB55E  # mGBA src/gba/core.c


def main(path: str) -> int:
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if len(data) < 0xA0:
        print(f"error: ROM too small ({len(data)} bytes)", file=sys.stderr)
        return 1

    logo = bytes(data[4:0xA0])
    if zlib.crc32(logo) != LOGO_CRC32:
        print(
            f"error: logo CRC {zlib.crc32(logo):08X} != {LOGO_CRC32:08X} "
            "(mGBA will skip the BIOS)", file=sys.stderr
        )
        return 1

    # Header checksum: byte 0xBD = two's complement of the sum of 0xA0..0xBC.
    s = sum(data[0xA0:0xBD]) & 0xFF
    data[0xBD] = (0x100 - s) & 0xFF

    # Pad to a multiple of 0x200 (min 0x200).
    size = max((len(data) + 0x1FF) & ~0x1FF, 0x200)
    data.extend(b"\xFF" * (size - len(data)))

    with open(path, "wb") as f:
        f.write(data)
    print(f"header fixed: checksum=0x{data[0xBD]:02X}, size={len(data)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
