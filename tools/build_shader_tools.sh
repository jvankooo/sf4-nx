#!/usr/bin/env bash
# Builds the two host tools of the shader library on Windows (Git Bash, LLVM clang++), into out/tools:
#   xenos_hlsl.exe       container -> HLSL (XenosRecomp with masseffect-nx's changes, shaders/me)
#   me_pack_shaders.exe  containers + SPIR-V -> sf4_shaders.mesp and its .idx (the renderer's library format)
# Then: python tools/build_shader_library.py
set -e
cd "$(dirname "$0")/.."
export PATH="/c/Program Files/LLVM/bin:$PATH"
M=shaders/me
mkdir -p out/tools
clang++ -std=c++20 -O2 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -Wno-switch -Wno-unused-variable -Wno-null-arithmetic \
  -fms-extensions -include $M/XenosRecomp/pch_min.h -I$M -I$M/XenosRecomp -Isdk/thirdparty/fmt/include \
  -Isdk/thirdparty/xxHash -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DMASSEFFECT_RECOMP \
  $M/tools/masseffect_hlsl.cpp $M/XenosRecomp/shader_recompiler.cpp -o out/tools/xenos_hlsl.exe
clang++ -std=c++20 -O2 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -Isdk/thirdparty/xxHash \
  $M/tools/me_pack_shaders.cpp app/src/native/masseffect/masseffect_shader_library.cpp -o out/tools/me_pack_shaders.exe
ls -l out/tools
