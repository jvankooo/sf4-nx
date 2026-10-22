// sf4-nx: the few libnx (<switch.h>) names the native renderer from masseffect-nx uses, for its PC build. Only on
// the include path of non-Switch builds (app/CMakeLists.txt). Threads are std::threads; the Horizon-only queries
// fail, so the callers keep their defaults.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using Result = uint32_t;
using Handle = uint32_t;

#define R_SUCCEEDED(res) ((res) == 0)
#define R_FAILED(res) ((res) != 0)
#define CUR_THREAD_HANDLE 0xFFFF8000u
#define CUR_PROCESS_HANDLE 0xFFFF8001u

enum { InfoType_TotalMemorySize = 6, InfoType_UsedMemorySize = 7 };

inline Result svcGetThreadPriority(s32* priority, Handle) {
  *priority = 0;
  return 1;
}

inline Result svcGetInfo(u64* value, u32, Handle, u64) {
  *value = 0;
  return 1;
}

// libnx's system tick: 19.2 MHz on the Switch; the same rate here, from the steady clock.
inline u64 armGetSystemTick() {
  return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now().time_since_epoch()).count() * 192 / 10000);
}
inline u64 armGetSystemTickFreq() { return 19200000; }

struct Thread {
  void (*entry)(void*) = nullptr;
  void* arg = nullptr;
};

inline Result threadCreate(Thread* t, void (*entry)(void*), void* arg, void*, size_t, int, int) {
  t->entry = entry;
  t->arg = arg;
  return 0;
}

inline Result threadStart(Thread* t) {
  std::thread(t->entry, t->arg).detach();
  return 0;
}
