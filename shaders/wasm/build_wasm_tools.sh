#!/usr/bin/env bash
# Builds two WebAssembly modules of the installer page (installer/), the same programs as tools/build_shader_tools.sh
# builds natively (masseffect-nx's translator and packer, under shaders/me):
#   hlsl.mjs  container -> HLSL translator (shaders/me/tools/masseffect_hlsl.cpp + XenosRecomp)
#   pack.mjs  package + index packer      (shaders/me/tools/me_pack_shaders.cpp + the app's shader library code)
# DXC (dxc_web) is not built here: installer/tools/fetch_dxc.sh downloads a pinned, checksummed WebAssembly build.
# Run it from a shell where Emscripten is active (em++ on PATH).
#
#   XXHASH_DIR  folder with xxhash.h        (default: sdk/thirdparty/xxHash)
#   FMT_DIR     folder that contains fmt/   (default: sdk/thirdparty/fmt/include)
#   OUT         output folder (default shaders/wasm/out)
#   usage: shaders/wasm/build_wasm_tools.sh [hlsl] [pack]     (default: both)
#
# Both are plain command line programs: the page writes the inputs into Emscripten's in-memory file system (FS) and
# calls callMain([...]) with the same arguments as on the command line, then reads the outputs back.
set -eu
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$here/../.." && pwd)
M=$ROOT/shaders/me
APP=$ROOT/app/src/native/masseffect
XXHASH_DIR=${XXHASH_DIR:-$ROOT/sdk/thirdparty/xxHash}
FMT_DIR=${FMT_DIR:-$ROOT/sdk/thirdparty/fmt/include}
OUT=${OUT:-$here/out}
command -v em++ >/dev/null || { echo "em++ not found: activate Emscripten first" >&2; exit 1; }
[ -f "$XXHASH_DIR/xxhash.h" ] || { echo "xxhash.h not found in $XXHASH_DIR: set XXHASH_DIR" >&2; exit 1; }
[ -f "$FMT_DIR/fmt/format.h" ] || { echo "fmt/format.h not found in $FMT_DIR: set FMT_DIR" >&2; exit 1; }
mkdir -p "$OUT"
# stackSave/stackRestore: callMain puts argv on the stack and never pops it; the page calls main once per shader on
# the same instance (installer/js/workers/handlers.js runMain restores the stack around each call).
COMMON=(-sEXPORT_ES6=1 -sMODULARIZE=1 -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=4MB -sEXPORTED_RUNTIME_METHODS=FS,callMain,stackSave,stackRestore -sENVIRONMENT=web,worker,node)
targets=${*:-hlsl pack}
for t in $targets; do
  echo "=== $t"
  case $t in
    hlsl)
      # -fexceptions: XenosRecomp rejects some shaders by throwing; masseffect_hlsl.cpp catches and reports them
      # ("translation rejected: <reason>"), which the page treats as a skip, as the native build does.
      em++ -std=c++20 -O1 -fexceptions -I"$M" -I"$M/XenosRecomp" -I"$FMT_DIR" -I"$XXHASH_DIR" -include "$M/XenosRecomp/pch_min.h" \
        -Wno-null-arithmetic -Wno-switch -Wno-unused-variable -fms-extensions -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DMASSEFFECT_RECOMP \
        "$M/tools/masseffect_hlsl.cpp" "$M/XenosRecomp/shader_recompiler.cpp" -o "$OUT/hlsl.mjs" \
        -sEXPORT_NAME=createHlslModule "${COMMON[@]}";;
    pack)
      em++ -std=c++20 -O2 -I"$XXHASH_DIR" "$M/tools/me_pack_shaders.cpp" "$APP/masseffect_shader_library.cpp" \
        -o "$OUT/pack.mjs" -sEXPORT_NAME=createPackModule -sMAXIMUM_MEMORY=4GB "${COMMON[@]}";;
    *) echo "unknown target $t" >&2; exit 1;;
  esac
done
echo "done: $OUT"
