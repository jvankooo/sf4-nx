#!/usr/bin/env python3
"""Reads the packets the native renderer logs with sf4_native_pm4_dump_swap=N (log lines "[native] pm4 ...") and
prints each draw and each resolve (EDRAM copy) of those frames with the registers that matter: render targets,
window offset, scissor, color mask, predication, the copy's source and destination, and the texture fetch constants.

    python tools/pm4_frame.py app/out/pcvk/logs/sf4_001.log [--draws] [--fetch]
"""
import re
import sys

R = dict(SURFACE=0x2000, COLOR0=0x2001, DEPTH=0x2002, COLOR1=0x2003, COLOR2=0x2004, COLOR3=0x2005,
         WINDOW=0x2080, SC_TL=0x2081, SC_BR=0x2082, MASK=0x2104, DEPTHCONTROL=0x2200, MODE=0x2208,
         COPY_CONTROL=0x2318, COPY_BASE=0x2319, COPY_PITCH=0x231A, COPY_INFO=0x231B, VPORT_XSCALE=0x210F)
FETCH = 0x4800
CONST_BASE = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908, 4: 0x2000}


def surf(v):
    return f"pitch {v & 0x3FFF} msaa {(v >> 16) & 3}"


def cinfo(v):
    return f"base {v & 0xFFF:03X} fmt {(v >> 16) & 0xF}"


def xy(v):
    return f"{v & 0x7FFF},{(v >> 16) & 0x7FFF}"


def signed15(v):
    v &= 0x7FFF
    return v - 0x8000 if v & 0x4000 else v


def fetch_text(regs, n):
    f = [regs.get(FETCH + n * 6 + i, 0) for i in range(6)]
    if not f[1]:
        return None
    w = (f[2] & 0x1FFF) + 1
    h = ((f[2] >> 13) & 0x1FFF) + 1
    return f"t{n}:{(f[1] >> 12) << 12:08X} {w}x{h} fmt{f[1] & 0x3F}{' tiled' if f[0] >> 31 else ' linear'}"


def main():
    path = sys.argv[1]
    show_draws = '--draws' in sys.argv
    show_fetch = '--fetch' in sys.argv
    regs = {}
    bin_mask = bin_select = (1 << 64) - 1
    line_re = re.compile(r"pm4 d(\d+) @([0-9A-F]+) ([0-9A-F]{8}) type (\d) .*?n(\d+):((?: [0-9A-F]{8})*)")
    n_draw = n_copy = 0
    swap = 0
    for line in open(path, encoding='utf-8', errors='replace'):
        m = line_re.search(line)
        if not m:
            continue
        header = int(m.group(3), 16)
        t = int(m.group(4))
        words = [int(x, 16) for x in m.group(6).split()]
        if t == 0:
            index = header & 0x7FFF
            single = (header >> 15) & 1
            for i, w in enumerate(words):
                regs[index if single else index + i] = w
            continue
        if t == 1:
            regs[header & 0x7FF] = words[0]
            regs[(header >> 11) & 0x7FF] = words[1]
            continue
        if t != 3:
            continue
        op = (header >> 8) & 0x7F
        pred = header & 1
        if '--bins' in sys.argv and op in (0x60, 0x61, 0x62, 0x63, 0x3F):
            names = {0x60: 'BIN_MASK_LO', 0x61: 'BIN_MASK_HI', 0x62: 'BIN_SELECT_LO', 0x63: 'BIN_SELECT_HI', 0x3F: 'IB'}
            print(f"    {names[op]} {' '.join('%08X' % w for w in words[:2])} (d{m.group(1)})")
        if '--bins' in sys.argv and op == 0x2D and words and ((words[0] >> 16) & 0xFF) == 4:
            idx = 0x2000 + (words[0] & 0x7FF)
            if idx <= 0x2082 < idx + len(words) - 1 or idx <= 0x2080 < idx + len(words) - 1:
                vals = ' '.join('%04X:%08X' % (idx + i, w) for i, w in enumerate(words[1:]))
                print(f"    SET_CONSTANT{' pred' if pred else ''} {vals} -> {'runs' if (not pred or (bin_select & bin_mask)) else 'skipped'}")
        if pred and not (bin_select & bin_mask) and op != 0x64:
            continue  # predicated out: the packet does nothing
        if op == 0x2D and words:  # SET_CONSTANT
            base = CONST_BASE.get((words[0] >> 16) & 0xFF)
            if base is not None:
                for i, w in enumerate(words[1:]):
                    regs[base + (words[0] & 0x7FF) + i] = w
        elif op == 0x60: bin_mask = (bin_mask & ~0xFFFFFFFF) | words[0]
        elif op == 0x61: bin_mask = (bin_mask & 0xFFFFFFFF) | (words[0] << 32)
        elif op == 0x62: bin_select = (bin_select & ~0xFFFFFFFF) | words[0]
        elif op == 0x63: bin_select = (bin_select & 0xFFFFFFFF) | (words[0] << 32)
        elif op == 0x64:
            print(f"==== SWAP {swap}: front {words[1]:08X} {words[2]}x{words[3]}")
            swap += 1
        elif op in (0x22, 0x36):
            initiator = words[1] if op == 0x22 else words[0]
            if initiator == 0x00010081:
                continue  # Direct3D's one-point draws
            passes = not pred or bool(bin_select & bin_mask)
            mode = regs.get(R['MODE'], 0) & 7
            common = (f"{surf(regs.get(R['SURFACE'], 0))} window {signed15(regs.get(R['WINDOW'], 0))},"
                      f"{signed15(regs.get(R['WINDOW'], 0) >> 16)} scissor {xy(regs.get(R['SC_TL'], 0))}-"
                      f"{xy(regs.get(R['SC_BR'], 0))}{'' if passes else ' PREDICATED-OUT'}"
                      f"{f' bins {bin_select & bin_mask:X}' if pred else ''}")
            if mode == 6:
                n_copy += 1
                cc = regs.get(R['COPY_CONTROL'], 0)
                src = cc & 7
                info = regs.get(R['DEPTH'] if src == 4 else [R['COLOR0'], R['COLOR1'], R['COLOR2'], R['COLOR3']][min(src, 3)], 0)
                dinfo = regs.get(R['COPY_INFO'], 0)
                print(f"COPY {n_copy}: src {'depth' if src == 4 else f'color{src}'} {cinfo(info)} {common} -> "
                      f"{regs.get(R['COPY_BASE'], 0):08X} pitch {regs.get(R['COPY_PITCH'], 0) & 0x3FFF}x"
                      f"{(regs.get(R['COPY_PITCH'], 0) >> 16) & 0x3FFF} destfmt {(dinfo >> 7) & 0x3F} "
                      f"{'' if (dinfo >> 3) & 1 else 'tiled'} cmd {(cc >> 20) & 3} clear {(cc >> 8) & 3}")
            else:
                n_draw += 1
                if show_draws:
                    mask = regs.get(R['MASK'], 0) & 0xFFFF
                    targets = []
                    for i, r in enumerate([R['COLOR0'], R['COLOR1'], R['COLOR2'], R['COLOR3']]):
                        if (mask >> (4 * i)) & 0xF:
                            targets.append(f"c{i} {cinfo(regs.get(r, 0))}")
                    dc = regs.get(R['DEPTHCONTROL'], 0)
                    if dc & 3:
                        targets.append(f"z {cinfo(regs.get(R['DEPTH'], 0))}")
                    fetches = ''
                    if show_fetch:
                        fetches = ' ' + ' '.join(x for x in (fetch_text(regs, k) for k in range(16)) if x)
                    extra = ''
                    if (initiator & 0x3F) == 8 and '--clears' in sys.argv:
                        import struct
                        fl = lambda r: struct.unpack('>f', regs.get(r, 0).to_bytes(4, 'big'))[0]
                        extra = (f" | mask {mask:04X} dc {dc:08X} vte {regs.get(0x2206, 0):08X} vport x {fl(0x210F):g}"
                                 f"+{fl(0x2110):g} y {fl(0x2111):g}+{fl(0x2112):g} clip {regs.get(0x2204, 0):08X}"
                                 f" vtx_cntl {regs.get(0x2302, 0):08X} f0 {regs.get(FETCH, 0):08X} {regs.get(FETCH + 1, 0):08X}")
                    print(f"  draw {n_draw}: init {initiator:08X} mode {mode} {' / '.join(targets) or 'nothing'} "
                          f"{common}{fetches}{extra}")
    print(f"{n_draw} draws, {n_copy} copies")


if __name__ == '__main__':
    main()
