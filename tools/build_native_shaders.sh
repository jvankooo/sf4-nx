#!/usr/bin/env bash
# Compiles the native renderer's own utility shaders (EDRAM transfers, resolves: app/src/native/masseffect/shaders,
# from masseffect-nx) to SPIR-V initializers (*.inc) in out/native-shaders, for both the PC and the Switch build
# (the devkitA64 build has no glslang). The list and flags are the ones of masseffect-nx's app/CMakeLists.txt.
# glslangValidator: out/glslang/StandAlone/glslangValidator.exe (built from sdk/thirdparty/glslang by
# out/build_glslang.bat), or GLSLANG=<path>.
set -e
cd "$(dirname "$0")/.."
GLSLANG=${GLSLANG:-out/glslang/StandAlone/glslangValidator.exe}
SRC=app/src/native/masseffect/shaders
OUT=out/native-shaders
mkdir -p "$OUT"
COMP="masseffect_edram_7e3_to_rgba8 masseffect_edram_rgba8_to_7e3 masseffect_edram_16f_to_16f me_resolve_exp_bias
me_edram_depth_to_rgba8 me_edram_depth_to_16f me_edram_raw64_to_rgba8 me_edram_rgba8_to_raw64 me_edram_raw64_to_16f
me_edram_16f_to_raw64 me_edram_raw64_to_raw64 me_edram_r16g16_to_rgba8 me_edram_rgba8_to_r16g16 me_edram_r16g16_to_16f
me_edram_16f_to_r16g16 me_edram_depth_to_raw64 me_depth_resolve_guestspace me_depth_resolve_guestspace_msaa2
me_edram_stencil_to_buffer me_edram_stencil_msaa2_to_buffer me_edram_color_stencil_to_buffer me_resolve_7e3_to_unorm10"
FRAG="me_edram_color_to_depth me_edram_depth_to_depth me_edram_depth_to_stencil me_edram_color_to_stencil
me_edram_depth_to_depth_msaa2 me_edram_depth_msaa2_to_depth_1x me_edram_depth_to_stencil_msaa2
me_edram_depth_msaa2_to_stencil_1x me_edram_raw64_to_depth me_edram_color_to_color me_edram_r64_to_r64
me_resolve_exp_bias_frag me_resolve_7e3_to_unorm10_frag me_resolve_7e3_word_frag me_resolve_fixed16_frag"
VK12="me_edram_stencil_to_buffer me_edram_color_stencil_to_buffer me_edram_stencil_msaa2_to_buffer
me_edram_depth_to_depth_msaa2 me_edram_depth_msaa2_to_depth_1x me_edram_depth_to_stencil_msaa2
me_edram_depth_msaa2_to_stencil_1x me_depth_resolve_guestspace_msaa2"
one() {  # one <name> <extension>
  local name=$1 ext=$2 out="$OUT/$1.inc" flags=()
  if [[ " $(echo $VK12) " == *" $name "* ]]; then flags=(--target-env vulkan1.2); out="$OUT/${name}_generated.inc"; fi
  "$GLSLANG" -V "${flags[@]}" -x -o "$out" "$SRC/$name.$ext" > /dev/null || { echo "FAILED $name"; return 1; }
}
n=0
for s in $COMP; do one "$s" comp; n=$((n + 1)); done
for s in $FRAG; do one "$s" frag; n=$((n + 1)); done
one me_edram_import vert; n=$((n + 1))
echo "$n shaders -> $OUT"
