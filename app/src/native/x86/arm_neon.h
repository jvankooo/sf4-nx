// sf4-nx: the native renderer from masseffect-nx uses ARM NEON intrinsics directly (its PC builds ran on Apple
// silicon). On an x86 PC this header stands in for the compiler's <arm_neon.h>: SIMDe implements the same intrinsics
// with SSE (or plain C). Only on the include path of non-ARM builds (app/CMakeLists.txt).
#pragma once
#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/arm/neon.h>
