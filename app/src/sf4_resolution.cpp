// SF4 - menus, movies and the HUD at 1280x720, the 3D scene at 854x480 (scaled up).
//
// WHY
//   SF4 sizes everything from one screen size: the main object reads the video mode at start-up (sub_82332D18:
//   1280x720 when HD widescreen) and the render manager (sub_8222F658) keeps it at +2496 (width) / +2500 (height).
//   The 3D scene's renderer picks its whole layout from that height: 720 = a 1280x720 scene in three predicated tiles
//   of 544 with 2x MSAA, 480 = a 640x480 scene in two tiles of 320 (sub_820D5560 creates the render targets, the
//   others choose per frame between the HD and SD variants). The console's GPU does about 22 ms of work per drawn
//   frame for the 480p scene, two thirds of it the scene itself: a 720p scene does not fit 30 fps. Everything 2D
//   (menus, HUD, text, movies) is cheap, and at 854x480 it was blurry.
//
// WHAT IT DOES (sf4_scene_480p, with video_mode 1280x720)
//   The scene renderer's functions that read the screen size (the 0x820C1BF0 / 0x820Dxxxx module below) see 854x480
//   while they run: the scene, its depth and its post-processing are the 480p ones, and the game draws the result
//   over the 1280x720 back buffer with a full-screen quad. The rest of the game (2D layout, HUD, menus, movies, the
//   back buffer) keeps 1280x720. Found on the PC (2026-10-23): with only sub_820D5560 at 480p the targets were 480p
//   but the per-frame tiling still used the 720p tiles (a seam at x 544); with the whole module at 480p the fight,
//   the VS and world-map screens, the movies and the menus are right.
//   The other readers of +2496/+2500 (0x8223D120.., 0x8244xxxx) are the 2D side and keep the real size.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstring>

REXCVAR_DEFINE_BOOL(sf4_scene_480p, true, "SF4",
                    "With video_mode 1280x720: the 3D scene is rendered at 854x480 and scaled up while menus, movies, "
                    "the HUD and text stay at 1280x720 (the GPU cost of a 480p game with a sharp picture)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kWidth = 2496, kHeight = 2500;  // render manager: the screen size
constexpr uint32_t kSceneWidth = 854, kSceneHeight = 480;

uint32_t Be32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}
void StoreBe32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

thread_local int t_depth = 0;  // nested scene functions: only the outermost one switches
std::atomic<bool> g_logged{false};

}  // namespace

REX_EXTERN(__imp__sub_8222F658);  // the render manager (singleton)

// Runs a scene function with the render manager's screen size at 854x480 (only when the screen is 720p or more).
template <void (*Original)(PPCContext&, uint8_t*)>
static void AsScene480(PPCContext& ctx, uint8_t* base) {
  if (t_depth > 0 || !REXCVAR_GET(sf4_scene_480p)) {
    Original(ctx, base);
    return;
  }
  const PPCRegister r3 = ctx.r3;
  __imp__sub_8222F658(ctx, base);
  const uint32_t manager = ctx.r3.u32;
  ctx.r3 = r3;
  const uint32_t w = Be32(base, manager + kWidth), h = Be32(base, manager + kHeight);
  if (h < 720) {
    Original(ctx, base);
    return;
  }
  if (!g_logged.exchange(true)) {
    REXLOG_INFO("[sf4] scene at {}x{}, screen {}x{} (sf4_scene_480p)", kSceneWidth, kSceneHeight, w, h);
  }
  StoreBe32(base, manager + kWidth, kSceneWidth);
  StoreBe32(base, manager + kHeight, kSceneHeight);
  ++t_depth;
  Original(ctx, base);
  --t_depth;
  StoreBe32(base, manager + kWidth, w);
  StoreBe32(base, manager + kHeight, h);
}

#define SF4_SCENE_FUNCTION(addr)    \
  REX_EXTERN(__imp__sub_##addr);    \
  REX_HOOK_RAW(sub_##addr) { AsScene480<__imp__sub_##addr>(ctx, base); }

SF4_SCENE_FUNCTION(820C1BF0)
SF4_SCENE_FUNCTION(820D1410)
SF4_SCENE_FUNCTION(820D2248)
SF4_SCENE_FUNCTION(820D2FA0)
SF4_SCENE_FUNCTION(820D30F8)  // per frame, from the scene step's drawing (sub_82168A80)
SF4_SCENE_FUNCTION(820D3588)
SF4_SCENE_FUNCTION(820D5560)  // creates the render targets
SF4_SCENE_FUNCTION(820D6DD0)
