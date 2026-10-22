// SF4 - 30 frames per second on screen, the game at full speed (two game steps per drawn frame).
//
// WHY
//   SF4's game logic is frame based: one step of everything (input, characters, round timer) per pass of the main
//   loop, and every pass also records the frame's drawing, plays it and presents. Console run 5 (2026-10-23): with
//   the main thread alone on its core, a fight still cost about 17.6 ms of CPU per pass against the 16.7 of 60 Hz,
//   so the game ran at ~39 steps per second (slow motion). sf4_draw_every_n_frames only skipped playing every other
//   frame; the game still recorded and presented all 60.
//
// HOW SF4'S FRAME IS BUILT (main loop sub_82332EA8 on the object of sub_82333318, vtable 0x820155F8)
//   +32 sub_823331F0  update: the task system once (sub_82334118 -> ... -> sub_82168A80, the scene step)
//   +36 sub_82332F40  play the recorded command buffer (sub_82333938 -> sub_82287DF0)
//   +28 sub_823339B8  frame limiter (target rate at [obj+44], 0 = off) and present (sub_822887A0 -> VdSwap)
//   The scene step sub_82168A80: unless the main object's +48 says "paused", the update calls
//     scene manager (sub_8216A160) +16 (sub_8216A218: also reads the pads, input manager +16),
//     current scene ([manager+4]) +4, scene manager +20 (sub_8216A2C0);
//   then the drawing of the frame (scene manager +24/+28, current scene +8, recorded into the command buffer).
//
// WHAT IT DOES (sf4_frame_rate = 30)
//   Before each scene step it runs the update calls again as many times as needed to keep 60 steps per real
//   second (normally once: two steps per drawn frame; up to sf4_max_steps_per_frame when a frame runs long, so a
//   slow frame never slows the game). The game's own limiter is set to 30 frames per second ([obj+44] = 30.0; only
//   the limiter reads it) and every frame is drawn. sf4_frame_rate = 60: the game's own loop, one step per frame (with
//   sf4_draw_every_n_frames).
//
// THE PADS
//   The pad manager (sub_82239598, vtable 0x8209E6A8) polls XInput once per frame (+12, sub_82238D20) and keeps per
//   pad the held buttons, the newly pressed ones and a "pressed with auto-repeat" mask (+52 of the entry), which the
//   menus use. The input manager (sub_820CA9F0, run in every step by the scene manager's update) copies them. Two
//   steps on one poll both saw the same new press: every button acted twice (console run 6, 2026-10-23). The pads
//   are polled again between the steps of a frame, so every step has its own poll, as at 60 frames per second
//   (the auto-repeat counts polls, so its speed is right again too).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_INT32(sf4_frame_rate, 30, "SF4",
                     "30 = the picture at 30 frames per second with the game at full speed (two game steps per drawn "
                     "frame); 60 = one game step per frame, as the game does (see sf4_draw_every_n_frames)")
    .range(30, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(sf4_max_steps_per_frame, 4, "SF4",
                     "With sf4_frame_rate = 30: at most this many game steps per drawn frame when a frame runs long "
                     "(more keeps full speed through slower frames; the picture rate drops instead)")
    .range(2, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kStep = std::chrono::nanoseconds(1000000000LL / 60);

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

// Calls the guest virtual function at slot `offset` of the object in r3.
void CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t offset) {
  ctx.r3.u64 = object;
  const uint32_t target = Load32(base, Load32(base, object) + offset);
  rex::runtime::ResolveIndirectFunction(target)(ctx, base);
}

Clock::time_point g_last{};
Clock::duration g_owed{};
uint64_t g_frames = 0, g_extra_steps = 0, g_max_steps = 0;
Clock::time_point g_report{};

}  // namespace

bool Sf4FrameRate30() { return REXCVAR_GET(sf4_frame_rate) == 30; }

REX_EXTERN(__imp__sub_82333318);  // the main object (singleton)
REX_EXTERN(__imp__sub_8216A160);  // the scene manager (singleton)
REX_EXTERN(__imp__sub_82239598);  // the pad manager (singleton)

// The pad manager's poll (+12): XInput state, new presses and auto-repeat of the four pads.
static void PollPads(PPCContext& ctx, uint8_t* base) {
  __imp__sub_82239598(ctx, base);
  CallVirtual(ctx, base, ctx.r3.u32, 12);
}

// One game step without drawing: the update half of sub_82168A80.
static void RunUpdateStep(PPCContext& ctx, uint8_t* base) {
  __imp__sub_82333318(ctx, base);
  const uint32_t main_object = ctx.r3.u32;
  CallVirtual(ctx, base, main_object, 48);  // paused?
  if (ctx.r3.u32 != 0) return;
  __imp__sub_8216A160(ctx, base);
  const uint32_t manager = ctx.r3.u32;
  CallVirtual(ctx, base, manager, 16);
  if (const uint32_t scene = Load32(base, manager + 4)) CallVirtual(ctx, base, scene, 4);
  CallVirtual(ctx, base, manager, 20);
}

// The scene step: the extra update steps this drawn frame owes, then the game's own step (update + drawing).
REX_EXTERN(__imp__sub_82168A80);
REX_HOOK_RAW(sub_82168A80) {
  if (!Sf4FrameRate30()) {
    __imp__sub_82168A80(ctx, base);
    return;
  }
  const auto now = Clock::now();
  uint32_t steps = 1;
  if (g_last != Clock::time_point{}) {
    g_owed += now - g_last;
    const uint32_t max_steps = uint32_t(REXCVAR_GET(sf4_max_steps_per_frame));
    steps = uint32_t(std::clamp<int64_t>(g_owed / kStep, 1, max_steps));
    // Time beyond the last step allowed is dropped (a load, a pause of the process): the game does not race.
    g_owed = std::clamp<Clock::duration>(g_owed - steps * kStep, -kStep, kStep);
  }
  g_last = now;

  if (steps > 1) {
    const PPCRegister r3 = ctx.r3;
    const uint32_t r1 = ctx.r1.u32;
    for (uint32_t i = 1; i < steps; ++i) {
      RunUpdateStep(ctx, base);
      PollPads(ctx, base);  // the next step sees its own presses
    }
    ctx.r3 = r3;
    ctx.r1.u64 = r1;
  }
  __imp__sub_82168A80(ctx, base);

  ++g_frames;
  g_extra_steps += steps - 1;
  g_max_steps = std::max<uint64_t>(g_max_steps, steps);
  if (now - g_report >= std::chrono::seconds(10)) {
    if (g_report != Clock::time_point{}) {
      REXLOG_INFO("[sf4] 30 fps mode, last 10 s: {} drawn frames, {} game steps ({:.1f} per second), at most {} per "
                  "frame", g_frames, g_frames + g_extra_steps,
                  double(g_frames + g_extra_steps) / std::chrono::duration<double>(now - g_report).count(),
                  g_max_steps);
    }
    g_frames = g_extra_steps = g_max_steps = 0;
    g_report = now;
  }
}

// The frame limiter and present: the game's own limiter paces the frames at 30 per second ([obj+44], frames per
// second as a float, 0 = no limit; nothing else reads it).
REX_EXTERN(__imp__sub_823339B8);
REX_HOOK_RAW(sub_823339B8) {
  if (Sf4FrameRate30()) {
    const float fps = 30.0f;
    uint32_t bits;
    std::memcpy(&bits, &fps, sizeof(bits));
    Store32(base, ctx.r3.u32 + 44, bits);
  }
  __imp__sub_823339B8(ctx, base);
}
