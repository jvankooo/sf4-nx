#!/usr/bin/env bash
# Assembles the static site (what GitHub Pages serves) from the installer sources and the build artifacts.
#
#   installer/tools/stage_site.sh <out dir> [--wasm <dir>] [--releases <dir>] [--require-complete]
#
#   --wasm <dir>       the WebAssembly tools (hlsl, pack, dxc_web: .mjs + .wasm). Default: installer/wasm
#   --releases <dir>   the release assets (sf4-nx.nro, sf4.toml) to publish next to the page; a manifest.json with
#                      their size and SHA-256 is written. Without them the page says that no build is published yet.
#   --require-complete fail unless the tools and the NRO are all there
#
# The page itself needs no build step: this only copies files and generates releases/manifest.json and
# wasm/runtime_containers.json.
set -eu
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
inst=$(cd "$here/.." && pwd)
root=$(cd "$inst/.." && pwd)
out=${1:?usage: stage_site.sh <out dir> [--wasm <dir>] [--releases <dir>] [--require-complete]}
shift
wasm=$inst/wasm
releases=
strict=0
while [ $# -gt 0 ]; do
  case $1 in
    --require-complete) strict=1; shift;;
    --wasm) wasm=$2; shift 2;;
    --releases) releases=$2; shift 2;;
    *) echo "unknown argument $1" >&2; exit 1;;
  esac
done

rm -rf "$out"
mkdir -p "$out/wasm" "$out/releases"
cp "$inst/index.html" "$inst/style.css" "$inst/config.js" "$out/"
cp -R "$inst/js" "$inst/assets" "$out/"
touch "$out/.nojekyll"
cp "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.md" "$out/"

# sf4.toml: the settings file the page puts into the zip (the release's own copy wins, below).
cp "$root/app/sf4.toml" "$out/sf4.toml"
# The translator's shared header, pasted into every generated HLSL by the translator.
cp "$root/shaders/me/XenosRecomp/shader_common.h" "$out/wasm/shader_common.h"
# The runtime shader containers (Direct3D immediate mode): SF4's and masseffect-nx's sets, as name -> base64.
python3 - "$root/shaders/sf4_runtime_containers" "$root/shaders/me/runtime_containers" "$out/wasm/runtime_containers.json" <<'PY'
import base64, json, os, sys
*sources, dst = sys.argv[1:]
data = {}
for src in sources:
    for f in sorted(os.listdir(src)):
        if f.endswith('.bin'):
            data.setdefault(f, base64.b64encode(open(os.path.join(src, f), 'rb').read()).decode('ascii'))
json.dump(data, open(dst, 'w'), indent=0)
print(f"runtime_containers.json: {len(data)} containers")
PY

n=0
for f in hlsl pack dxc_web; do
  for ext in mjs wasm; do
    if [ -f "$wasm/$f.$ext" ]; then cp "$wasm/$f.$ext" "$out/wasm/"; n=$((n + 1)); fi
  done
done
echo "staged $n wasm files from $wasm"
[ "$n" -eq 6 ] || echo "WARNING: expected 6 wasm files (hlsl, pack, dxc_web: .mjs + .wasm); the page will report the missing ones" >&2

if [ -n "$releases" ]; then
  for asset in "$releases"/*.nro; do [ ! -f "$asset" ] || cp "$asset" "$out/releases/"; done
  # The settings must belong to the same release as the executable.
  [ ! -f "$releases/sf4.toml" ] || cp "$releases/sf4.toml" "$out/sf4.toml"
fi
if [ "$strict" -eq 1 ]; then
  [ "$n" -eq 6 ] || { echo 'error: incomplete WebAssembly toolchain' >&2; exit 1; }
  [ -s "$out/releases/sf4-nx.nro" ] || { echo 'error: missing release asset sf4-nx.nro' >&2; exit 1; }
fi
cp "$out/sf4.toml" "$out/releases/.toml-for-manifest"
python3 - "$out" "${RELEASE_TAG:-}" <<'PY'
import hashlib, json, os, sys
out, tag = sys.argv[1], sys.argv[2]
assets = {}
rel = os.path.join(out, "releases")
for name in sorted(os.listdir(rel)):
    path = os.path.join(rel, name)
    if name == ".toml-for-manifest":
        data = open(path, "rb").read()
        assets["sf4.toml"] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        os.remove(path)
    elif name.endswith(".nro"):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        assets[name] = {"size": os.path.getsize(path), "sha256": h.hexdigest()}
json.dump({"tag": tag or None, "assets": assets}, open(os.path.join(rel, "manifest.json"), "w"), indent=1)
print("manifest.json:", ", ".join(assets) or "(no assets)")
PY
echo "site staged in $out"
