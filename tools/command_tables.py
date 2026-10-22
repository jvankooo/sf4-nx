#!/usr/bin/env python3
"""Prints the opcode tables of the command-buffer player sub_82287DF0, with what the trace measured per handler.

sub_82287DF0(device, buffer) walks the recorded commands: header word = opcode << 16 | length in words, 0x00010000
ends a segment. For each command it calls tableA[opcode](device, cmd) and, when that returns non-zero,
tableB[opcode](device, cmd). The tables are words of the image at 0x82500980 (A) and 0x82500790 (B).

    python tools/command_tables.py IMAGE_DUMP [rex_d3d.log --report N]

IMAGE_DUMP is the guest image written with SF4_DUMP_IMAGE (src/sf4_app.h). With a trace log, each handler gets its
calls per frame in that report.
"""
import argparse
import re
import struct

IMAGE_BASE = 0x82000000
TABLE_A = 0x82500980
TABLE_B = 0x82500790
CODE_LO, CODE_HI = 0x820C0000, 0x824E3CC4


def main():
    p = argparse.ArgumentParser()
    p.add_argument("image")
    p.add_argument("log", nargs="?")
    p.add_argument("--report", type=int, default=0)
    a = p.parse_args()
    image = open(a.image, "rb").read()
    word = lambda addr: struct.unpack_from(">I", image, addr - IMAGE_BASE)[0]
    per_frame = {}
    if a.log:
        blocks = re.split(r"^(?===== report )", open(a.log, errors="replace").read(), flags=re.M)[1:]
        for line in (blocks[a.report - 1] if a.report else blocks[-1]).splitlines():
            m = re.match(r"^sub_([0-9A-F]{8})\s+(?:API)?\s+\d+\s+([\d.]+)", line)
            if m:
                per_frame[int(m.group(1), 16)] = float(m.group(2))
    print("op   table A (first)      per frame   table B (if A != 0)  per frame")
    op = 0
    while True:
        fa, fb = word(TABLE_A + 4 * op), word(TABLE_B + 4 * op)
        if not (CODE_LO <= fa < CODE_HI and CODE_LO <= fb < CODE_HI):
            break
        print(f"{op:3d}  sub_{fa:08X} {per_frame.get(fa, 0):12.2f}   sub_{fb:08X} {per_frame.get(fb, 0):12.2f}")
        op += 1
    print(f"{op} opcodes")


if __name__ == "__main__":
    main()
