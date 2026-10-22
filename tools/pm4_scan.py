#!/usr/bin/env python3
"""Lists every recompiled function that builds PM4 type-3 packet headers, in one pass over the generated code.

Same detection as tools/pm4_ops.py (a lis + ori/addi pair whose 32-bit value has the type-3 bits 0b11 at the top),
but over all functions at once, for finding the game's Direct3D: the draw functions build DRAW_INDX /
DRAW_INDX_2, the state functions SET_CONSTANT, the resolve EVENT_WRITE and so on.

    python tools/pm4_scan.py [--gen app/generated/default] [--op DRAW_INDX] [--lo 0x82000000 --hi 0x83000000]
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pm4_ops import OPS  # noqa: E402  (pm4_ops parses argv at import only through its loop, which sees no names)

DEF = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
LIS = re.compile(r"^\s*// lis (r\d+),(-?\d+)$")
LOW = re.compile(r"^\s*// (ori|addi) (r\d+),(r\d+),(-?\d+)$")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    p = argparse.ArgumentParser()
    p.add_argument("--gen", default=os.path.join(root, "app", "generated", "default"))
    p.add_argument("--op", action="append", default=[], help="only functions that build this opcode (name)")
    p.add_argument("--lo", type=lambda s: int(s, 16), default=0)
    p.add_argument("--hi", type=lambda s: int(s, 16), default=0xFFFFFFFF)
    a = p.parse_args()
    found = {}
    for path in glob.glob(os.path.join(a.gen, "*_recomp.*.cpp")):
        cur, hi = None, {}
        for line in open(path, errors="replace"):
            m = DEF.match(line)
            if m:
                addr = int(m.group(1), 16)
                cur = addr if a.lo <= addr < a.hi else None
                hi = {}
                continue
            if cur is None:
                continue
            m = LIS.match(line)
            if m:
                hi[m.group(1)] = (int(m.group(2)) & 0xFFFF) << 16
                continue
            m = LOW.match(line)
            if m and m.group(3) in hi:
                v = (hi[m.group(3)] + int(m.group(4))) & 0xFFFFFFFF
                if v >> 30 == 3:
                    op = (v >> 8) & 0x7F
                    found.setdefault(cur, []).append(f"{OPS.get(op, hex(op))}[{((v >> 16) & 0x3FFF) + 1}]")
    for addr in sorted(found):
        ops = found[addr]
        if a.op and not any(o.split("[")[0] in a.op for o in ops):
            continue
        print(f"sub_{addr:08X} {' '.join(ops)}")


if __name__ == "__main__":
    main()
