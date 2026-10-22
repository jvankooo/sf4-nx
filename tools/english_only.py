#!/usr/bin/env python3
"""Lists, or removes, the files of a game_root that only the non-English languages use.

SF4's disc carries every language: per-language text and UI folders (ENG, JPN, FRA, GER, ITA, SPA, KOR, in
Battle/Hud, Game/Menu, Game/Window, Game/CmdList and the movie subtitles) and two voice sets (character and
narration files ending in _E for English and _J for Japanese). The movies themselves are shared by all languages.
With the console language English (the SDK's user_language = 1, its default) and English voices, the game needs
only the ENG folders and the _E voices.

    python tools/english_only.py build/sf4-nx/game_root            # list what would go and its size
    python tools/english_only.py build/sf4-nx/game_root --apply    # remove it

Run it on the SD copy, never on assets/game_root: the code generator and the PC tests use the full disc.
--keep-japanese-voices keeps the _J voice files (the in-game option to switch a character to Japanese).
"""
import argparse
import os
import re
import shutil

OTHER_LANGUAGES = {"JPN", "FRA", "GER", "ITA", "SPA", "KOR"}
JAPANESE_VOICE = re.compile(r"_J\.(csb|cpk)$", re.I)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("root")
    p.add_argument("--apply", action="store_true")
    p.add_argument("--keep-japanese-voices", action="store_true")
    a = p.parse_args()
    if os.path.abspath(a.root).replace("\\", "/").rstrip("/").endswith("assets/game_root"):
        raise SystemExit("refusing to trim assets/game_root: run it on the SD copy (build/sf4-nx/game_root)")
    dirs, files = [], []
    for dp, dn, fn in os.walk(a.root):
        for d in list(dn):
            if d.upper() in OTHER_LANGUAGES:
                dirs.append(os.path.join(dp, d))
                dn.remove(d)
        for f in fn:
            if not a.keep_japanese_voices and JAPANESE_VOICE.search(f):
                files.append(os.path.join(dp, f))

    def size(path):
        if os.path.isfile(path):
            return os.path.getsize(path)
        return sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(path) for f in fs)

    total = sum(size(x) for x in dirs + files)
    whole = size(a.root)
    print(f"{len(dirs)} language folders and {len(files)} Japanese voice files: {total / 2**20:.1f} MB "
          f"of {whole / 2**30:.2f} GB")
    for d in dirs:
        print(f"  {size(d) / 2**20:7.1f} MB  {os.path.relpath(d, a.root)}")
    if files:
        print(f"  {sum(size(f) for f in files) / 2**20:7.1f} MB  {len(files)} files *_J.csb / *_J.cpk")
    if a.apply:
        for d in dirs:
            shutil.rmtree(d)
        for f in files:
            os.remove(f)
        print(f"removed; game_root is now {size(a.root) / 2**30:.2f} GB")


if __name__ == "__main__":
    main()
