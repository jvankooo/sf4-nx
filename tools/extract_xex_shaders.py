#!/usr/bin/env python3
"""Extracts the shader containers embedded in SF4's executable (118: engine and effect shaders that are not in
Shader/*.emb), from a dump of the loaded guest image, for the shader library of the native renderer.

    SF4_DUMP_IMAGE=out/sf4_image.bin  (run the game for a few seconds: sf4_app.h writes the image once loaded)
    python tools/extract_xex_shaders.py out/sf4_image.bin assets/shaders_xex

A container is accepted with the runtime's layout checks (shaders/me/tools/container_check.h): signature
0x102A1100 (pixel) or 0x102A1101 (vertex), 24 <= virtual size <= 0x40000, 0 < physical size <= 0x40000, and the
shader header and its microcode inside the container. Writes <out>/xex_<guest address>.<ps|vs>.bin.
The output is made from the user's disc: keep it out of any repository (assets/ is ignored).
"""
import os
import struct
import sys

IMAGE_BASE = 0x82000000


def container_size(d, i):
    if i + 28 > len(d) or d[i + 3] not in (0, 1):
        return 0
    virtual, physical = struct.unpack_from(">II", d, i + 4)
    if not (24 <= virtual <= 0x40000 and 0 < physical <= 0x40000 and i + virtual + physical <= len(d)):
        return 0
    header = struct.unpack_from(">I", d, i + 24)[0]
    if header + 8 > virtual:
        return 0
    offset, size = struct.unpack_from(">II", d, i + header)
    if not size or offset + size > physical:
        return 0
    return virtual + physical


def main():
    image, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    d = open(image, "rb").read()
    count = 0
    i = d.find(b"\x10\x2a\x11")
    while i >= 0:
        size = container_size(d, i)
        if size:
            kind = "vs" if d[i + 3] else "ps"
            with open(os.path.join(out, f"xex_{IMAGE_BASE + i:08X}.{kind}.bin"), "wb") as f:
                f.write(d[i:i + size])
            count += 1
        i = d.find(b"\x10\x2a\x11", i + 1)
    print(f"{count} containers -> {out}")


if __name__ == "__main__":
    main()
