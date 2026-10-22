#!/usr/bin/env python3
"""Prints the PowerPC listing of the recompiled function that contains an address, from the generated code.

    python tools/listing.py 0x82287E70 [--around 40]

With --around N only the N instructions before and after the address are printed. Instruction addresses are
counted from the function start (one "// <asm>" comment per instruction), the way tools/find_gaps.py does.
"""
import argparse
import bisect
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "app", "generated", "default")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("address", type=lambda s: int(s, 16))
    p.add_argument("--around", type=int, default=0)
    a = p.parse_args()
    table = open(os.path.join(GEN, "sf4_init.cpp"), errors="replace").read()
    starts = sorted(int(x, 16) for x in re.findall(r"\{ 0x([0-9A-F]{8}), sub_", table))
    start = starts[bisect.bisect_right(starts, a.address) - 1]
    name = f"sub_{start:08X}"
    for path in glob.glob(os.path.join(GEN, "sf4_recomp.*.cpp")):
        lines = open(path, errors="replace").read().splitlines()
        for i, line in enumerate(lines):
            if line.startswith(f"DEFINE_REX_FUNC({name})"):
                asm = []
                for body in lines[i + 1:]:
                    if body.startswith("}"):
                        break
                    m = re.match(r"^\t// (\S.*)$", body)
                    if m and not m.group(1).startswith(("PPC_", "0x", "ERROR")):
                        asm.append(m.group(1))
                print(f"{name} ({len(asm)} instructions, {os.path.basename(path)}:{i + 1})")
                k = (a.address - start) // 4
                lo, hi = (max(0, k - a.around), min(len(asm), k + a.around + 1)) if a.around else (0, len(asm))
                for j in range(lo, hi):
                    mark = ">>" if j == k else "  "
                    print(f"{mark} {start + 4 * j:08X}  {asm[j]}")
                return
    print(f"{name} not found in the generated code")


if __name__ == "__main__":
    main()
