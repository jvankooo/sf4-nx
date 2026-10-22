// sf4-nx: what the native renderer core (me_native_system.cpp and native/masseffect, from masseffect-nx) uses from
// files that are made for Mass Effect and not built here.
//   me_ring_wait.cpp      hooks Mass Effect's Direct3D GPU wait: SF4's own version is sf4_ring_wait.cpp.
//   me_hang_watchdog.cpp  a diagnostic of Mass Effect's threads.
//   me_resolution.cpp     Mass Effect's internal resolution, set in its engine: SF4 sizes everything from the video
//                         mode (video_mode_width / height), so these keep "the game's own size".
//   me_audio_hooks.cpp    defines the vblank setting the graphics system reads.
// On the PC also the Switch SDK's profiler counters and xxHash's out-of-line functions.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/platform.h>

#include "me_native_system.h"

REXCVAR_DEFINE_INT32(masseffect_scene_width, 0, "Mass Effect",
                     "Mass Effect's internal render width; unused by SF4 (0 = the game's own size)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_scene_height, 0, "Mass Effect",
                     "Mass Effect's internal render height; unused by SF4 (0 = the game's own size)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_scene_parts, 0, "Mass Effect",
                     "Parts of Mass Effect's internal-resolution change that apply; none in SF4")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_vblank_sleep_exact, true, "Mass Effect",
                    "Native GPU vblank thread: sleep to the next vblank instead of waking every 1 ms")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// No internal-resolution crop of the output (me_resolution.cpp's answer when its back buffer part is off).
extern "C" bool MeResolutionOutputSize(uint32_t*, uint32_t*) { return false; }

namespace me::native {

void StartHangWatchdog() {}

void HangWatchdogInterval(uint64_t, uint64_t) {}

}  // namespace me::native

#if !REX_PLATFORM_SWITCH
// The Switch SDK's profiler (src/ui/switch_perf.cpp) is not in the PC build.
extern "C" void RexSwitchPerfCount(unsigned) {}
extern "C" void RexSwitchPerfAdd(unsigned, uint64_t) {}
extern "C" bool RexSwitchPerfSkip(unsigned) { return false; }
extern "C" void RexSwitchPerfHitch(uint64_t, uint64_t) {}

// xxHash's functions for the files that include it without XXH_INLINE_ALL (on the Switch the SDK links them).
#define XXH_IMPLEMENTATION
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#endif
