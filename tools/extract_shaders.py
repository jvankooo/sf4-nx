#!/usr/bin/env python3
"""Extracts SF4's shader containers from game_root/Shader/*.emb, for the shader library of the native renderer.

SF4 keeps all its shaders in four Capcom EMB archives (PsUserShader, VsUserShader, PsBgShader, VsBgShader). An EMB
starts with "#EMB", the entry count at +12, the entry table offset at +0x18 and the name table offset at +0x1C. The
entry table has one (offset, size) pair of big-endian words per entry, the offset counted from the entry's own
position in the table; the name table one word per entry, an offset from the start of the file to a C string.
Each entry is a standard Xbox 360 XDK
shader container (first word 0x102A1100 pixel shader, 0x102A1101 vertex shader), the input of XenosRecomp as in
nfsmw-nx and masseffect-nx.

    python tools/extract_shaders.py assets/game_root/Shader assets/shaders

Writes <out>/<archive>_<index>.<ps|vs>.bin and <out>/index.tsv (archive, index, kind, size, the first 16 bytes of
the SHA-1 of the container, and the entry's name from the archive). The output is made from the user's disc: keep it out of any
repository (assets/ is ignored).
"""
import hashlib
import os
import struct
import sys


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    rows = []
    for name in sorted(os.listdir(src)):
        if not name.lower().endswith(".emb"):
            continue
        data = open(os.path.join(src, name), "rb").read()
        if data[:4] != b"#EMB":
            print(f"{name}: not an EMB archive, skipped")
            continue
        count = struct.unpack_from(">I", data, 12)[0]
        table, names = struct.unpack_from(">II", data, 0x18)
        kinds = {"ps": 0, "vs": 0}
        for i in range(count):
            entry = table + 8 * i
            offset, size = struct.unpack_from(">II", data, entry)
            start = entry + offset
            label = ""
            if names:
                name_at = struct.unpack_from(">I", data, names + 4 * i)[0]
                if 0 < name_at < len(data):
                    label = data[name_at:data.index(b"\0", name_at)].decode("ascii", "replace")
            blob = data[start:start + size]
            magic = struct.unpack_from(">I", blob, 0)[0] if len(blob) >= 4 else 0
            kind = {0x102A1100: "ps", 0x102A1101: "vs"}.get(magic)
            if kind is None:
                print(f"{name} #{i}: not a shader container (first word {magic:08X}), skipped")
                continue
            kinds[kind] += 1
            base = os.path.splitext(name)[0]
            with open(os.path.join(out, f"{base}_{i:03d}.{kind}.bin"), "wb") as f:
                f.write(blob)
            rows.append((base, i, kind, size, hashlib.sha1(blob).hexdigest()[:32], label))
        print(f"{name}: {count} entries, {kinds['ps']} pixel and {kinds['vs']} vertex shaders")
    with open(os.path.join(out, "index.tsv"), "w", newline="\n") as f:
        f.write("archive\tindex\tkind\tsize\tsha1_128\tname\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")
    print(f"{len(rows)} shader containers -> {out}")


if __name__ == "__main__":
    main()
