// sf4 - the game's command stream (see sf4_commands.h).

#include "sf4_commands.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <chrono>
#include <thread>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstring>

// The game's logic is locked to 60 frames per second (a fighting game): slowing the whole game down would make the
// fights slower. This draws only one frame in N instead: the game still runs and reads the pads 60 times per
// second, and the command buffers of the other frames are not played, so the GPU (and the SDK's Xenos emulation)
// does 1/N of the work and the last drawn image stays on screen. The final image is resolved into the same texture
// every frame (docs/d3d-map.md), so the frames not drawn show the previous one.
REXCVAR_DEFINE_INT32(sf4_draw_every_n_frames, 2, "SF4",
                     "Draw one frame in N (1 = every frame, 60 fps; 2 = 30 fps). The game itself keeps running at "
                     "60 frames per second; only the drawing is skipped")
    .range(1, 4);

namespace sf4::commands {
namespace {

constexpr const char* kNames[kOpcodes] = {
    "unused", "unused", "Data", "Callback", "SetRenderTarget", "SetDepthStencilSurface", "unused",
    "RestoreDefaultTargets", "Clear", "SetViewport", "ColorWriteMask", "ZEnable", "ZWriteEnable", "ZFunc",
    "AlphaTestEnable", "AlphaFuncRef", "StencilEnable", "StencilFuncRefMask", "StencilWriteMask", "StencilOps",
    "FillMode", "CullMode", "DepthBias", "BlendControl", "SamplerMisc", "SamplerAddress", "SamplerFilter",
    "SamplerMaxAnisotropy", "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP",
    "SetStreamSource", "SetIndices", "SetVertexDeclaration", "SetVertexShader", "SetPixelShader",
    "SetVertexShaderConstantF", "SetVertexShaderConstantI", "SetVertexShaderConstantB", "SetPixelShaderConstantF",
    "SetPixelShaderConstantI", "SetPixelShaderConstantB", "SetTexture", "ClipPlaneEnable", "SetClipPlane", "unused",
    "unused", "unused", "unused", "Resolve", "BeginTiling", "EndTiling", "SetPredication", "SetShaderGPRAllocation",
    "ClearStateBits", "SetStateBits", "LoadPixelShaderConstantF", "LoadVertexShaderConstantF", "unused", "unused",
};

constexpr int kMaxObservers = 4;
std::atomic<PlayerObserver> g_observers[kMaxObservers];
std::atomic<PlayerObserver> g_after_observers[kMaxObservers];
std::atomic<DrawObserver> g_draw_observers[kMaxObservers];
thread_local bool t_playing = false;  // this thread is inside the player

template <typename T>
void Add(std::atomic<T>* slots, T observer) {
  for (int i = 0; i < kMaxObservers; ++i) {
    T empty = nullptr;
    if (slots[i].compare_exchange_strong(empty, observer)) return;
  }
}

}  // namespace

void NotifyDraw(DrawFunction function, uint32_t type, uint32_t first, uint32_t second, uint32_t count) {
  if (!t_playing) return;
  const DrawCall call{function, type, first, second, count};
  for (auto& slot : g_draw_observers) {
    if (const auto observer = slot.load(std::memory_order_acquire)) observer(call);
  }
}

const char* OpName(uint32_t op) { return op < kOpcodes ? kNames[op] : "?"; }

bool Readable(uint32_t address, uint32_t bytes) {
  auto* memory = rex::system::kernel_memory();
  // 64-bit end: words such as 0xFFFFFFFF must not wrap around to a "readable" low address.
  const uint64_t end = uint64_t(address) + bytes;
  if (!memory || address < 0x00010000 || (address & 3) || end > 0x100000000ull) return false;
  for (uint64_t a = address & ~0xFFFu; a < end; a += 0x1000) {
    auto* heap = memory->LookupHeap(uint32_t(a));
    // Committed and readable. The protection alone is not enough: a decommitted page keeps its last protection
    // in the page table, and reading it faulted on the PC (an "unhandled guest access violation" in the tracer).
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(a), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) || !(info.protect & rex::memory::kMemoryProtectRead)) {
      return false;
    }
  }
  return true;
}

#if REX_PLATFORM_SWITCH
// The Switch memory layer's always-mapped alias of guest memory (guest_memory_switch.cpp): a page the GPU emulation
// is watching, or not faulted in yet, is unmapped in the normal view but readable here.
extern "C" void* RexGmShadowFor(uint64_t window_address);
#endif

uint32_t Word(uint32_t address) {
  const void* host = rex::system::kernel_memory()->TranslateVirtual(address);
#if REX_PLATFORM_SWITCH
  if (const void* shadow = RexGmShadowFor(reinterpret_cast<uint64_t>(host))) host = shadow;
#endif
  uint32_t raw;
  std::memcpy(&raw, host, 4);
  return __builtin_bswap32(raw);
}

float Float(uint32_t address) {
  const uint32_t w = Word(address);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

void AddPlayerObserver(PlayerObserver observer) { Add(g_observers, observer); }
void AddAfterPlayObserver(PlayerObserver observer) { Add(g_after_observers, observer); }
void AddDrawObserver(DrawObserver observer) { Add(g_draw_observers, observer); }

}  // namespace sf4::commands

namespace cmd = sf4::commands;

namespace {
// Frames presented so far: calls of sub_822887A0, the game's once-per-frame call that ends in VdSwap
// (docs/d3d-map.md, "The frame").
std::atomic<uint64_t> g_presents{0};
}  // namespace

REXCVAR_DEFINE_INT32(sf4_debug_slow_frame_ms, 0, "SF4",
                     "Testing: make every frame this many milliseconds longer on the game thread (frame pacing)")
    .range(0, 100);
REXCVAR_DEFINE_INT32(sf4_debug_slow_draw_ms, 0, "SF4",
                     "Testing: make each drawn frame this many milliseconds longer on the game thread (frame pacing)")
    .range(0, 100);

// How long the game thread spends playing the drawn frames' command buffers (the game's Direct3D building the GPU
// commands), logged every 10 s: the part of the game thread's frame that sf4_draw_every_n_frames saves.
namespace {
uint64_t g_play_ns = 0, g_play_count = 0, g_play_max_ns = 0;
std::chrono::steady_clock::time_point g_play_report{};
void NotePlayTime(std::chrono::steady_clock::duration d) {
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
  g_play_ns += ns;
  g_play_max_ns = std::max(g_play_max_ns, ns);
  ++g_play_count;
  const auto now = std::chrono::steady_clock::now();
  if (g_play_report == std::chrono::steady_clock::time_point{}) g_play_report = now;
  if (now - g_play_report < std::chrono::seconds(10)) return;
  REXLOG_INFO("[sf4] command player, last 10 s: {} drawn frames, {:.2f} ms each on the game thread (max {:.2f})",
              g_play_count, g_play_count ? double(g_play_ns) / 1e6 / double(g_play_count) : 0.0,
              double(g_play_max_ns) / 1e6);
  g_play_ns = g_play_count = g_play_max_ns = 0;
  g_play_report = now;
}
}  // namespace

REX_EXTERN(__imp__sub_822887A0);
REX_HOOK_RAW(sub_822887A0) {
  g_presents.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_822887A0(ctx, base);
}

// The handler of op 3 (callback): it calls a function of the table at 0x82619480 with the command.
REX_EXTERN(__imp__sub_8228A4F8);

// The player of the command buffer: sub_82287DF0(player, buffer). The observers look at the buffer, then the
// original plays it. The addresses of this file appear in the source, so tools/direct_calls.py keeps the calls to
// these functions hookable on the Switch.
void Sf4NoteMainThread();  // sf4_jobs.cpp: the profiler's main-thread mark and exclusive core
bool Sf4FrameRate30();     // sf4_framerate.cpp: 30 fps with two game steps per frame, every frame drawn

REX_EXTERN(__imp__sub_82287DF0);
REX_HOOK_RAW(sub_82287DF0) {
  Sf4NoteMainThread();
  const uint32_t buffer = ctx.r4.u32;
  // sf4_debug_slow_frame_ms: every frame (drawn or not) costs this much more on the game thread (frame pacing tests).
  if (const int slow = REXCVAR_GET(sf4_debug_slow_frame_ms); slow > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(slow));
  }
  // sf4_draw_every_n_frames: in the frames not drawn the buffer is not played. Only its callbacks (op 3) run, in
  // case the game waits for what they do; nothing else in a buffer has an effect outside the GPU state, which the
  // player resets at the start of every buffer anyway (it fills its state cache, player+212, before playing).
  const int every = Sf4FrameRate30() ? 1 : REXCVAR_GET(sf4_draw_every_n_frames);
  if (every > 1 && g_presents.load(std::memory_order_relaxed) % uint64_t(every) != 0) {
    const uint32_t player = ctx.r3.u32;
    cmd::ForEachCommand(buffer, [&](uint32_t op, uint32_t command, uint32_t) {
      if (op != 3) return;
      ctx.r3.u64 = player;
      ctx.r4.u64 = command;
      __imp__sub_8228A4F8(ctx, base);
    });
    return;
  }
  for (auto& slot : cmd::g_observers) {
    if (const auto observer = slot.load(std::memory_order_acquire)) observer(buffer);
  }
  // sf4_debug_slow_draw_ms: a drawn frame costs this much more on the game thread (tests frame pacing on a PC).
  if (const int slow = REXCVAR_GET(sf4_debug_slow_draw_ms); slow > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(slow));
  }
  cmd::t_playing = true;
  const auto play_start = std::chrono::steady_clock::now();
  __imp__sub_82287DF0(ctx, base);
  NotePlayTime(std::chrono::steady_clock::now() - play_start);
  cmd::t_playing = false;
  for (auto& slot : cmd::g_after_observers) {
    if (const auto observer = slot.load(std::memory_order_acquire)) observer(buffer);
  }
}

// The draw functions, for the observers that check what the handlers really pass (the frame decoder's
// verification). Arguments as the handlers of ops 28-31 set them up.
REX_EXTERN(__imp__sub_8228B258);
REX_HOOK_RAW(sub_8228B258) {
  cmd::NotifyDraw(cmd::kDrawVertices, ctx.r4.u32, ctx.r5.u32, 0, ctx.r6.u32);
  __imp__sub_8228B258(ctx, base);
}
REX_EXTERN(__imp__sub_8228B648);
REX_HOOK_RAW(sub_8228B648) {
  cmd::NotifyDraw(cmd::kDrawIndexedVertices, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  __imp__sub_8228B648(ctx, base);
}
REX_EXTERN(__imp__sub_8228A8D0);
REX_HOOK_RAW(sub_8228A8D0) {
  cmd::NotifyDraw(cmd::kBeginVertices, ctx.r4.u32, 0, ctx.r6.u32, ctx.r5.u32);
  __imp__sub_8228A8D0(ctx, base);
}
REX_EXTERN(__imp__sub_8228AD68);
REX_HOOK_RAW(sub_8228AD68) {
  cmd::NotifyDraw(cmd::kBeginIndexedVertices, ctx.r4.u32, 0, 0, ctx.r7.u32);
  __imp__sub_8228AD68(ctx, base);
}
