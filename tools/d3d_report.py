#!/usr/bin/env python3
"""Reads rex_d3d.log (src/sf4_d3d_trace.cpp) and prints one report as a table, with the first calls of each function.

    python tools/d3d_report.py app/out/pc/rex_d3d.log --report 10 [--api] [--min 0.5] [--detail 2]

--api keeps only functions called from outside the library (the entry points the game calls); --min drops those
called fewer times per frame; --detail N prints the first N recorded calls of each listed function.
"""
import argparse
import re


def main():
    p = argparse.ArgumentParser()
    p.add_argument("log")
    p.add_argument("--report", type=int, default=0, help="report number (0 = last)")
    p.add_argument("--api", action="store_true")
    p.add_argument("--min", type=float, default=0.0)
    p.add_argument("--detail", type=int, default=0)
    p.add_argument("--commands", action="store_true", help="print the report's command-stream section instead")
    p.add_argument("--op", type=int, action="append", default=[], help="with --commands: only these opcodes")
    a = p.parse_args()
    text = open(a.log, errors="replace").read()
    blocks = re.split(r"^(?===== report )", text, flags=re.M)[1:]
    block = blocks[a.report - 1] if a.report else blocks[-1]
    if a.commands:
        section = block[block.find("-- command stream"):]
        keep = True
        for line in section.splitlines():
            m = re.match(r"^op +(\d+):", line)
            if m:
                keep = not a.op or int(m.group(1)) in a.op
            if keep or line.startswith("--"):
                print(line)
        return
    details = {}
    for m in re.finditer(r"^(sub_[0-9A-F]{8}) lr=(.*)$", text, re.M):
        details.setdefault(m.group(1), []).append(m.group(2))
    print(block.splitlines()[0])
    for line in block.splitlines()[2:]:
        m = re.match(r"^(sub_[0-9A-F]{8})\s+(API)?\s+(\d+)\s+([\d.]+)\s+(\d+)\s*(.*)$", line)
        if not m:
            continue
        name, api, per_frame, callers = m.group(1), m.group(2), float(m.group(4)), m.group(6)
        if (a.api and not api) or per_frame < a.min:
            continue
        print(f"{name} {api or '   '} {per_frame:9.2f}/frame  {callers[:110]}")
        for d in details.get(name, [])[: a.detail]:
            print(f"      lr={d[:150]}")


if __name__ == "__main__":
    main()
