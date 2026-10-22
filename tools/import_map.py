#!/usr/bin/env python3
"""Map of which kernel/XAM imports the recompiled functions call, per address block.

Statically linked XDK libraries (Direct3D, XAudio, XMV...) sit in contiguous address ranges and use their own
imports (Direct3D: Vd*, Mm*Physical*, Ke*...). Printing, per block, how many functions call each import family shows
where a library starts and ends.

    python tools/import_map.py [--block 0x4000] [--lo 0x82180000 --hi 0x82260000] [--family Vd,MmAllocatePhysical]
"""
import argparse
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--block", type=lambda s: int(s, 16), default=0x4000)
    p.add_argument("--lo", type=lambda s: int(s, 16), default=0x820C0000)
    p.add_argument("--hi", type=lambda s: int(s, 16), default=0x824E4000)
    p.add_argument("--family", default="Vd,Mm,Ke,Xam,XAudio,Xma,Nt,Rtl,Ex,Ob,Io,Xex")
    a = p.parse_args()
    fams = a.family.split(",")
    funcs = json.load(open(os.path.join(ROOT, "out", "callgraph.json")))
    rows = {}
    for name, info in funcs.items():
        m = re.match(r"sub_([0-9A-F]{8})$", name)
        if not m:
            continue
        addr = int(m.group(1), 16)
        if not (a.lo <= addr < a.hi):
            continue
        blk = addr - addr % a.block
        r = rows.setdefault(blk, {"n": 0, **{f: 0 for f in fams}, "names": set()})
        r["n"] += 1
        for c in info["calls"]:
            if c.startswith("sub_"):
                continue
            for f in fams:
                if c.startswith(f):
                    r[f] += 1
                    r["names"].add(c)
                    break
    print("block       funcs " + " ".join(f"{f:>6}" for f in fams))
    for blk in sorted(rows):
        r = rows[blk]
        tag = " ".join(sorted(r["names"]))[:90]
        print(f"0x{blk:08X} {r['n']:6} " + " ".join(f"{r[f]:6}" for f in fams) + "  " + tag)


if __name__ == "__main__":
    main()
