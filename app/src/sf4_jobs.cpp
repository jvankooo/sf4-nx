// SF4 - the job system's idle workers give the CPU back.
//
// WHY
//   SF4 runs its per-frame work (animation, effects, ...) as jobs on five Job Worker threads (guest entry
//   0x820C0EC8). Each worker, once woken, loops "while jobs are pending, pop one" (sub_820C1510: an atomic exchange
//   of the queue head at [scheduler+20]); the main thread's scheduler (sub_820C0B48) pops the same way. While the
//   last jobs of a batch still run, the queue is empty but the pending count is not, so every idle worker polls
//   without pause. On the Xbox 360's six hardware threads that costs nothing; the Switch gives the game three cores,
//   and the five pollers have the same priority as the main thread and as the worker running the job.
//   Console run 2 (2026-10-23), fights: Job Workers 55 % of a core each, main thread 4.7 %, ring thread 4.7 %, GPU
//   idle 180 ms per Swap: 5 Swaps per second. In console run 1 the generated code still took a global lock around
//   every guest atomic sequence (skip_msr = false), which made the pollers sleep in the kernel: 10 % each, 30 fps.
//
// WHAT IT DOES
//   With sf4_job_wait_yield (default on), a pop that finds the queue empty first yields the core a few times; on a
//   Job Worker thread it then sleeps 50 us per poll (the Switch; Windows only yields, its sleeps are a millisecond or
//   more). The main thread only yields: it waits for its own jobs there, and a sleep would add to its frame. The game
//   sees the same queue, only polled less often.
//
// MAIN THREAD CORE
//   Console run 3 (2026-10-23), fights at 35.6 game frames per second: the main thread was the limit at 75 % of a
//   core (about 21 ms per frame), sharing cores with the ring thread (priority 0x2C, above it), the audio mixer and the
//   workers. sf4_exclusive_core gives it a core of its own: the SDK profiler (switch_perf.cpp, ApplyExclusiveCore)
//   pins the thread marked as the game's main thread to that core and moves every other thread off it. SF4's main
//   thread is marked from the command player (sf4_commands.cpp), which only it runs; the profiler's fallback without
//   that mark would have pinned the audio mixer (the hottest thread named "XThread...").
//
// MAIN THREAD SLEEP(0)
//   Console run 4 (2026-10-23, stack samples), fights at 43 game frames per second with the main thread alone on
//   core 2 and still busy only 74 % of the time: 28 % of its samples were in nanosleep, called from Sleep(0)
//   (sub_822AF8E0) in the job scheduler's wait loop (sub_820C0B48: help pop a job, Sleep(0), check again). On the
//   360 Sleep(0) only yields; the SDK's XThread::Delay turns a zero delay of a thread below normal priority, which
//   SF4's main thread is, into a 100 us host sleep, so every pass of that loop slept. With sf4_main_sleep0_yield,
//   Sleep(0) on the main thread yields instead (its core is its own: nothing waits for it there).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

REXCVAR_DEFINE_INT32(sf4_exclusive_core, 2, "SF4",
                     "Switch: the game's main thread gets this core (0-2) to itself, every other thread moves off it; "
                     "-1 = off")
    .range(-1, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sf4_stack_sampling, false, "SF4",
                    "Switch diagnosis: the profiler samples every thread's stack (logs/rex/rex_profile.log shows where "
                    "each thread spends its time); costs some speed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(sf4_main_sleep0_yield, true, "SF4",
                    "Sleep(0) on the game's main thread yields as on the 360 instead of sleeping 100 us (the SDK's rule "
                    "for threads below normal priority); its job-wait loop calls it every pass");

REXCVAR_DEFINE_BOOL(sf4_job_wait_yield, true, "SF4",
                    "Job system: a worker that finds the job queue empty gives the CPU back (yield, then 50 us "
                    "sleeps) instead of polling at full speed; the Switch has 3 cores for SF4's 5 workers");

namespace {

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));  // heap objects: below the 0xE0000000 view
  return __builtin_bswap32(v);
}

thread_local uint32_t t_empty_polls = 0;
thread_local bool t_job_worker = false;
thread_local bool t_main_thread = false;

}  // namespace

#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchPerfNoteThreadRole(unsigned role);
extern "C" void RexSwitchPerfExclusiveCore(int core);
#endif

// Called by the command player on the game's main thread.
void Sf4NoteMainThread() {
  if (t_main_thread) return;
  t_main_thread = true;
#if REX_PLATFORM_SWITCH
  RexSwitchPerfNoteThreadRole(0);
  static bool applied = false;
  if (!applied) {
    applied = true;
    const int core = REXCVAR_GET(sf4_exclusive_core);
    RexSwitchPerfExclusiveCore(core);
    REXLOG_INFO("[sf4] main thread marked for the profiler; exclusive core {}", core);
  }
#endif
}

// Sleep(ms) = SleepEx(ms, FALSE). Sleep(0) on the main thread: a yield, as on the 360.
REX_EXTERN(__imp__sub_822AF8E0);
REX_HOOK_RAW(sub_822AF8E0) {
  if (ctx.r3.u32 == 0 && t_main_thread && REXCVAR_GET(sf4_main_sleep0_yield)) {
    std::this_thread::yield();
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_822AF8E0(ctx, base);
}

// The Job Worker thread entry: marks the thread, then runs the worker loop (it never returns).
REX_EXTERN(__imp__sub_820C0EC8);
REX_HOOK_RAW(sub_820C0EC8) {
  t_job_worker = true;
  __imp__sub_820C0EC8(ctx, base);
}

// sub_820C1510(scheduler, worker data): pops one job from [scheduler+20] and runs it.
REX_EXTERN(__imp__sub_820C1510);
REX_HOOK_RAW(sub_820C1510) {
  if (REXCVAR_GET(sf4_job_wait_yield)) {
    if (Load32(base, ctx.r3.u32 + 20) == 0) {
      if (++t_empty_polls <= 8 || !t_job_worker) {
        std::this_thread::yield();
      } else {
#if REX_PLATFORM_SWITCH
        std::this_thread::sleep_for(std::chrono::microseconds(50));
#else
        std::this_thread::yield();
#endif
      }
    } else {
      t_empty_polls = 0;
    }
  }
  __imp__sub_820C1510(ctx, base);
}
