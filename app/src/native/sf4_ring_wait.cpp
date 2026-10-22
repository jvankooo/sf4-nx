// SF4 - the Direct3D wait for the GPU without spinning (native renderer), as masseffect-nx's me_ring_wait.cpp.
//
// WHY
//   sub_82216850 is the XDK Direct3D check "has the GPU read pointer moved since the last iteration" (the same routine
//   as Mass Effect's sub_8222FA98, other device offsets): 32 db16cyc delays, then it compares the word the GPU writes
//   the read pointer back to ([[device+10896]]) with the value seen last time ([r3+8]), and after 5000 ticks without
//   change declares the GPU hung. Its callers loop on it, so the game's main thread spins while the ring thread works
//   through the commands: on a 3-core Switch both want a core. On the PC the main thread showed ~97 % of a core.
//
// WHAT IT DOES
//   If the read pointer is unchanged, it waits at most sf4_wait_ring_max_us for the ring thread to write it back again
//   (NotifyRingProgress), then calls the original, which decides as before. The game sees the same values, only
//   later. With bit 0x02 of [device+10941] (GPU already declared hung) or without the native renderer, it does not
//   wait.

#include "me_native_system.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_BOOL(sf4_wait_blocking_ring, true, "SF4",
                    "Native renderer: the game's Direct3D sleeps while it waits for the ring thread to move the read "
                    "pointer instead of spinning in sub_82216850")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sf4_wait_ring_max_us, 2000, "SF4",
                     "Native renderer: longest sleep per iteration of the game's Direct3D GPU wait, in microseconds")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::native {
namespace {

std::mutex g_progress_mutex;
std::condition_variable g_progress_cv;
std::atomic<uint32_t> g_progress{0};
std::atomic<int> g_waiting{0};

}  // namespace

// The ring thread bumps the counter after writing the read pointer back, and checks for waiters under the lock (an
// unlocked handshake lost wake-ups on ARM in masseffect-nx).
void NotifyRingProgress() {
  g_progress.fetch_add(1, std::memory_order_acq_rel);
  bool notify;
  {
    std::lock_guard<std::mutex> lock(g_progress_mutex);
    notify = g_waiting.load(std::memory_order_acquire) > 0;
  }
  if (notify) g_progress_cv.notify_all();
}

}  // namespace me::native

namespace {

constexpr uint32_t kOffReadPointerWord = 10896;  // device: pointer to the word the GPU writes RPTR to
constexpr uint32_t kOffState = 10941;            // device: bit 0x02 = GPU considered hung

// As the generated code's REX_LOAD_U32: on Windows the 0xE0000000 physical view sits 4 KB further in host memory (the
// fence word Direct3D polls is there); on the Switch every view maps 1:1 onto base.
uint32_t HostOffset(uint32_t address) {
#if REX_PLATFORM_WIN32
  return address >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)address;
  return 0;
#endif
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address + HostOffset(address), sizeof(v));
  return __builtin_bswap32(v);
}

std::atomic<uint64_t> g_iterations{0}, g_sleeps{0}, g_advanced{0}, g_ns_asleep{0};
std::atomic<int64_t> g_next_report_ms{0};

void Report() {
  using namespace std::chrono;
  const int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  int64_t next = g_next_report_ms.load(std::memory_order_relaxed);
  if (now < next) return;
  if (!g_next_report_ms.compare_exchange_strong(next, now + 10000)) return;
  if (!next) return;  // the first call only arms the timer
  const uint64_t sleeps = g_sleeps.exchange(0), advanced = g_advanced.exchange(0);
  REXLOG_INFO("[ring_wait] last 10 s: {} D3D iterations, {} sleeps ({} ended by ring progress), {:.1f} ms asleep",
              g_iterations.exchange(0), sleeps, advanced, double(g_ns_asleep.exchange(0)) / 1e6);
}

}  // namespace

REX_EXTERN(__imp__sub_82216850);
REX_HOOK_RAW(sub_82216850) {
  static const bool active = me::native::Enabled() && REXCVAR_GET(sf4_wait_blocking_ring);
  if (active) {
    const uint32_t wait = ctx.r3.u32;
    const uint32_t device = wait ? Load32(base, wait) : 0;
    if (device && !(base[device + kOffState + HostOffset(device)] & 0x02)) {
      const uint32_t word = Load32(base, device + kOffReadPointerWord);
      if (word) {
        g_iterations.fetch_add(1, std::memory_order_relaxed);
        const uint32_t seen = me::native::g_progress.load(std::memory_order_acquire);
        if (Load32(base, word) == Load32(base, wait + 8)) {
          const auto before = std::chrono::steady_clock::now();
          g_sleeps.fetch_add(1, std::memory_order_relaxed);
          bool advanced;
          {
            std::unique_lock<std::mutex> lock(me::native::g_progress_mutex);
            me::native::g_waiting.fetch_add(1, std::memory_order_seq_cst);
            advanced = me::native::g_progress_cv.wait_for(
                lock, std::chrono::microseconds(REXCVAR_GET(sf4_wait_ring_max_us)),
                [seen] { return me::native::g_progress.load(std::memory_order_seq_cst) != seen; });
            me::native::g_waiting.fetch_sub(1, std::memory_order_acq_rel);
          }
          if (advanced) g_advanced.fetch_add(1, std::memory_order_relaxed);
          g_ns_asleep.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - before).count()),
                                std::memory_order_relaxed);
        }
        Report();
      }
    }
  }
  __imp__sub_82216850(ctx, base);
}

// The Swap's frame throttle: sub_82212798(device, fence, reason) waits until the fence word the ring writes back
// ([[device+10896]]) has reached `fence`; the kick counter is [device+10908]. Reason 3 comes from the Swap
// (sub_8221A058), with the fence of the previous Swap ([device+14940]). The previous Swap's work is kicked at a
// VBlank, so this is where the game thread waits for the next VBlank: sf4_vblank_catchup fires it at once when the
// game is behind a steady 60 Hz schedule (me_native_system.cpp CatchUpVblank).
REX_EXTERN(__imp__sub_82212798);
REX_HOOK_RAW(sub_82212798) {
  if (ctx.r5.u32 == 3 && me::native::Enabled()) {
    const uint32_t device = ctx.r3.u32, fence = ctx.r4.u32;
    const uint32_t word = device ? Load32(base, device + kOffReadPointerWord) : 0;
    if (word) {
      const uint32_t kick = Load32(base, device + 10908);
      if (kick - fence < kick - Load32(base, word)) me::native::RequestVblankCatchup();  // it is going to wait
    }
  }
  __imp__sub_82212798(ctx, base);
}
