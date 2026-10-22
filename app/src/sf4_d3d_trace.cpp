// ===========================================================================
//  PHASE A OF THE NATIVE RENDERER: MAP THE GAME'S DIRECT3D
//
//  SF4 links the XDK's Direct3D statically, in two parts: 0x8220C000-0x8222BFFF (tools/import_map.py: the users
//  of the Vd*, Mm*Physical* and KeGetCurrentProcessType imports; tools/callgraph.py: the functions around them
//  that call into or are called from that core) and 0x82287000-0x8228B7FF (it calls into the core, holds the
//  functions that build DRAW_INDX packets, tools/pm4_scan.py, and the caller of the swap). Nothing names those
//  functions, so, as masseffect-nx's me_d3d_trace.cpp
//  with MASSEFFECT_D3D_TRACE_ALL, this file wraps every one of them (sf4_d3d_trace_all.inc, written by
//  tools/d3d_trace_list.py) and records, like nfsmw-nx's nfsmw_d3d_trace.cpp:
//
//    - how many times each function is called, in total and per frame
//    - whether it is called from outside the library (an API entry point the game calls) or only from inside
//    - the most frequent return addresses (who calls it)
//    - the arguments (r3-r10, f1) and the return value of its first calls
//    - the command stream the game recorded (see WalkCommands): commands per opcode and sample payloads
//
//  A frame is one call of sub_8221A058, the only caller of VdSwap. The report goes to rex_d3d.log next to the
//  executable every 10 seconds while the game draws. With sf4_d3d_trace off (the default) every wrapper only
//  reads one flag and calls the original, so the game behaves exactly as without this file.
//
//  HOW THE HOOKING WORKS
//  Each recompiled function sub_X is a weak alias of __imp__sub_X (DEFINE_REX_FUNC in sf4_pch.h). The strong
//  sub_X defined here wins, so every call goes through it, direct or through the indirect-call table, and
//  __imp__sub_X is still the original. Calls that tools/direct_calls.py turned into __imp__sub_X skip the
//  wrapper: the PC build's own codegen step regenerates without them, so on PC every call is seen. On the
//  Switch this file is not built (SF4_D3D_TRACE).
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "sf4_commands.h"

#include <cstring>

REXCVAR_DEFINE_BOOL(sf4_d3d_trace, false, "SF4",
                    "Record the calls to the game's Direct3D (0x8220C000-0x8222BFFF and 0x82287000-0x8228B7FF) in "
                    "rex_d3d.log every 10 s "
                    "(phase A of the native renderer: which function is which)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// The two parts of the library (tools/d3d_trace_list.py): the core, and the cluster with the draw functions and
// the caller of the swap. A call from outside both is a call from the game.
struct Range {
  uint32_t lo, hi;
};
constexpr Range kRanges[] = {{0x8220C000, 0x8222C000}, {0x82287000, 0x8228B800}};

constexpr bool InLibrary(uint32_t address) {
  for (const Range& r : kRanges) {
    if (address >= r.lo && address < r.hi) return true;
  }
  return false;
}
constexpr uint32_t kFrameFunction = 0x8221A058;  // the only caller of VdSwap
constexpr size_t kMaxDetail = 6;                // first calls recorded per function
constexpr size_t kMaxCallers = 8;               // distinct callers per function
constexpr uint64_t kAllCallersUpTo = 4096;      // every call updates the callers until then, then 1 in 256
constexpr int kReportSeconds = 10;

#define X(a) 0x##a,
constexpr uint32_t kAddresses[] = {
#include "sf4_d3d_trace_all.inc"
};
#undef X
constexpr size_t kCount = sizeof(kAddresses) / sizeof(kAddresses[0]);

constexpr size_t IndexOf(uint32_t address) {
  size_t lo = 0, hi = kCount;  // kAddresses is sorted (tools/d3d_trace_list.py)
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (kAddresses[mid] < address) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

struct Detail {
  uint32_t lr;
  uint32_t r[8];  // r3..r10
  double f1;
  uint32_t ret;  // r3 on return
  uint32_t thread;
  std::atomic<bool> done{false};  // set after ret: the report only prints finished calls
};

struct Caller {
  uint32_t lr;
  uint64_t n;
};

struct Slot {
  std::atomic<uint64_t> calls{0};
  uint64_t reported = 0;  // calls already counted in a report (report thread only)
  std::atomic<uint32_t> details{0};
  Detail detail[kMaxDetail]{};
  Caller callers[kMaxCallers]{};
  uint64_t other_callers = 0;
  bool from_outside = false;  // some caller lies outside the library: an entry point the game calls
  bool detail_written = false;
};

Slot g_slots[kCount];
std::mutex g_callers_mutex;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_init{false};
std::mutex g_init_mutex;
std::atomic<uint64_t> g_frames{0};
uint64_t g_frames_reported = 0;
std::chrono::steady_clock::time_point g_last_report;
int g_report_number = 0;

void WalkCommands(uint32_t buffer);  // the command sampler, below

uint32_t ThreadTag() {
  static std::atomic<uint32_t> next{1};
  thread_local uint32_t tag = next.fetch_add(1, std::memory_order_relaxed);
  return tag;
}

inline bool Enabled() {
  if (!g_init.load(std::memory_order_acquire)) [[unlikely]] {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (!g_init.load(std::memory_order_relaxed)) {
      g_enabled.store(REXCVAR_GET(sf4_d3d_trace), std::memory_order_relaxed);
      g_last_report = std::chrono::steady_clock::now();
      if (g_enabled.load(std::memory_order_relaxed)) {
        sf4::commands::AddPlayerObserver(&WalkCommands);
        REXLOG_INFO("[d3d] tracing {} Direct3D functions (0x{:08X}-0x{:08X}, 0x{:08X}-0x{:08X}) to rex_d3d.log",
                    kCount, kRanges[0].lo, kRanges[0].hi - 1, kRanges[1].lo, kRanges[1].hi - 1);
      }
      g_init.store(true, std::memory_order_release);
    }
  }
  return g_enabled.load(std::memory_order_relaxed);
}

void NoteCaller(Slot& s, uint32_t lr) {
  std::lock_guard<std::mutex> lock(g_callers_mutex);
  if (!InLibrary(lr)) {
    s.from_outside = true;
  }
  for (auto& c : s.callers) {
    if (c.lr == lr) {
      ++c.n;
      return;
    }
    if (c.n == 0) {
      c.lr = lr;
      c.n = 1;
      return;
    }
  }
  ++s.other_callers;
}

// ---------------------------------------------------------------------------------------------------------------
// The command stream (docs/d3d-map.md): sub_82287DF0(player, buffer) plays the commands the game recorded. Every
// kSampleEvery-th buffer, before the original plays it (an observer of the hook in sf4_commands.cpp), the buffer is
// walked and each opcode gets its count and up to kSamples payloads per report, with the first words behind every
// payload word that is a readable guest address (resource headers: textures, surfaces, shaders...).
namespace cmd = sf4::commands;
constexpr uint32_t kOpcodes = cmd::kOpcodes;
constexpr uint32_t kSampleEvery = 30;  // buffers (frames) between two walks
constexpr size_t kSamples = 6;
constexpr size_t kPayloadWords = 16;
constexpr size_t kPointedWords = 6;

struct CommandSample {
  uint32_t length = 0;
  uint32_t words[kPayloadWords]{};
  uint32_t pointed[kPayloadWords][kPointedWords]{};
  uint32_t pointer_mask = 0;  // bit k: words[k] is a readable address and pointed[k] holds what it points to
};

struct OpcodeStats {
  uint64_t commands = 0;  // in the walked buffers of this report
  uint32_t min_length = ~0u, max_length = 0;
  size_t samples = 0;
  CommandSample sample[kSamples];
  uint64_t first_seen_walk = 0;
  uint32_t in_walk = 0;     // occurrences in the current walk
  uint32_t last_walk = 0;   // occurrences in the previous walk
};

OpcodeStats g_ops[kOpcodes];
uint64_t g_walks = 0;           // buffers walked in this report
uint64_t g_walk_commands = 0;   // commands in them
uint64_t g_walk_errors = 0;     // buffers abandoned (unreadable or malformed)
std::atomic<uint64_t> g_player_calls{0};

void WalkCommands(uint32_t buffer) {
  if (g_player_calls.fetch_add(1, std::memory_order_relaxed) % kSampleEvery != 0) return;
  std::lock_guard<std::mutex> lock(g_callers_mutex);
  ++g_walks;
  for (auto& o : g_ops) {
    if (o.in_walk) o.last_walk = o.in_walk;
    o.in_walk = 0;
  }
  const bool whole = cmd::ForEachCommand(buffer, [](uint32_t op, uint32_t command, uint32_t length) {
    ++g_walk_commands;
    OpcodeStats& o = g_ops[op];
    ++o.commands;
    o.min_length = std::min(o.min_length, length);
    o.max_length = std::max(o.max_length, length);
    // One sample per walk, at a different occurrence each time (spread over the previous walk's count), so the
    // samples are not all the first command of the frame.
    const uint32_t occurrence = o.in_walk++;
    const uint32_t wanted = o.last_walk ? uint32_t((g_walks * 7919u) % o.last_walk) : 0;
    const bool take = o.samples < kSamples && o.first_seen_walk != g_walks && occurrence == wanted;
    if (take && cmd::Readable(command, 4 * std::min<uint32_t>(length, kPayloadWords + 1))) {
      CommandSample& c = o.sample[o.samples++];
      o.first_seen_walk = g_walks;
      c = CommandSample{};
      c.length = length;
      for (uint32_t k = 0; k + 1 < length && k < kPayloadWords; ++k) {
        c.words[k] = cmd::Word(command + 4 + 4 * k);
        const uint32_t v = c.words[k];
        if (v >= 0x40000000 && cmd::Readable(v, 4 * kPointedWords)) {
          c.pointer_mask |= 1u << k;
          for (size_t j = 0; j < kPointedWords; ++j) c.pointed[k][j] = cmd::Word(v + 4 * uint32_t(j));
        }
      }
    }
  });
  if (!whole) ++g_walk_errors;
}

// Called from WriteReport with g_callers_mutex held.
void WriteCommands(FILE* f) {
  std::fprintf(f, "-- command stream: %llu buffers walked, %.1f commands per buffer, %llu abandoned --\n",
               (unsigned long long)g_walks, g_walks ? double(g_walk_commands) / double(g_walks) : 0.0,
               (unsigned long long)g_walk_errors);
  for (uint32_t op = 0; op < kOpcodes; ++op) {
    OpcodeStats& o = g_ops[op];
    if (!o.commands) continue;
    std::fprintf(f, "op %2u: %8.2f per buffer, length %u-%u words\n", op,
                 g_walks ? double(o.commands) / double(g_walks) : 0.0, o.min_length, o.max_length);
    for (size_t s = 0; s < o.samples; ++s) {
      const CommandSample& c = o.sample[s];
      std::string line;
      for (uint32_t k = 0; k + 1 < c.length && k < kPayloadWords; ++k) {
        char b[16];
        std::snprintf(b, sizeof(b), " %08X", c.words[k]);
        line += b;
      }
      std::fprintf(f, "      [%u]%s\n", c.length, line.c_str());
      for (uint32_t k = 0; k < kPayloadWords; ++k) {
        if (!(c.pointer_mask & (1u << k))) continue;
        std::fprintf(f, "         w%u -> %08X %08X %08X %08X %08X %08X\n", k, c.pointed[k][0], c.pointed[k][1],
                     c.pointed[k][2], c.pointed[k][3], c.pointed[k][4], c.pointed[k][5]);
      }
    }
  }
  for (auto& o : g_ops) {
    const uint32_t last = o.last_walk;
    o = OpcodeStats{};
    o.last_walk = last;
  }
  g_walks = g_walk_commands = g_walk_errors = 0;
}

std::string ReportPath() {
  const auto folder = rex::filesystem::GetExecutableFolder();
  return folder.empty() ? std::string("rex_d3d.log") : rex::path_to_utf8(folder / "rex_d3d.log");
}

void WriteReport(uint64_t frames, double seconds) {
  FILE* f = std::fopen(ReportPath().c_str(), g_report_number == 0 ? "w" : "a");
  if (!f) {
    return;
  }
  ++g_report_number;
  struct Row {
    size_t i;
    uint64_t window;
  };
  std::vector<Row> rows;
  for (size_t i = 0; i < kCount; ++i) {
    const uint64_t total = g_slots[i].calls.load(std::memory_order_relaxed);
    if (total) {
      rows.push_back({i, total - g_slots[i].reported});
      g_slots[i].reported = total;
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.window > b.window; });
  std::fprintf(f, "\n==== report %d: %llu frames in %.1f s (%.1f fps), %zu of %zu functions called so far ====\n",
               g_report_number, (unsigned long long)frames, seconds, frames / std::max(seconds, 0.001), rows.size(),
               kCount);
  std::fprintf(f, "%-12s %5s %10s %10s %12s  %s\n", "function", "api", "window", "per frame", "total", "callers");
  std::lock_guard<std::mutex> lock(g_callers_mutex);
  for (const Row& r : rows) {
    Slot& s = g_slots[r.i];
    std::string callers;
    for (const auto& c : s.callers) {
      if (!c.n) break;
      char b[40];
      std::snprintf(b, sizeof(b), " %08X:%llu", c.lr, (unsigned long long)c.n);
      callers += b;
    }
    if (s.other_callers) {
      callers += " +" + std::to_string(s.other_callers) + " more";
    }
    std::fprintf(f, "sub_%08X %5s %10llu %10.2f %12llu %s\n", kAddresses[r.i], s.from_outside ? "API" : "",
                 (unsigned long long)r.window, frames ? double(r.window) / double(frames) : 0.0,
                 (unsigned long long)s.calls.load(std::memory_order_relaxed), callers.c_str());
  }
  // First calls of every function seen for the first time in this report.
  bool header = false;
  for (const Row& r : rows) {
    Slot& s = g_slots[r.i];
    if (s.detail_written) continue;
    s.detail_written = true;
    if (!header) {
      std::fprintf(f, "-- first calls (lr, thread, r3..r10, f1 -> r3) --\n");
      header = true;
    }
    for (uint32_t k = 0; k < kMaxDetail; ++k) {
      const Detail& d = s.detail[k];
      if (!d.done.load(std::memory_order_acquire)) continue;
      std::fprintf(f, "sub_%08X lr=%08X t%u r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X "
                      "f1=%g -> %08X\n",
                   kAddresses[r.i], d.lr, d.thread, d.r[0], d.r[1], d.r[2], d.r[3], d.r[4], d.r[5], d.r[6], d.r[7],
                   d.f1, d.ret);
    }
  }
  WriteCommands(f);
  std::fclose(f);
}

void OnFrame() {
  const uint64_t frames = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto now = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(now - g_last_report).count();
  if (seconds < kReportSeconds) {
    return;
  }
  g_last_report = now;
  WriteReport(frames - g_frames_reported, seconds);
  g_frames_reported = frames;
}

template <uint32_t kAddress>
inline void Enter(PPCContext& ctx, uint32_t& detail_index) {
  constexpr size_t i = IndexOf(kAddress);
  static_assert(i < kCount && kAddresses[i] == kAddress);
  Slot& s = g_slots[i];
  const uint64_t n = s.calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (n < kAllCallersUpTo || (n & 255) == 0) {
    NoteCaller(s, lr);
  }
  detail_index = kMaxDetail;
  if (n < kMaxDetail) {
    detail_index = s.details.fetch_add(1, std::memory_order_relaxed);
    if (detail_index < kMaxDetail) {
      Detail& d = s.detail[detail_index];
      d.lr = lr;
      d.thread = ThreadTag();
      d.r[0] = ctx.r3.u32, d.r[1] = ctx.r4.u32, d.r[2] = ctx.r5.u32, d.r[3] = ctx.r6.u32;
      d.r[4] = ctx.r7.u32, d.r[5] = ctx.r8.u32, d.r[6] = ctx.r9.u32, d.r[7] = ctx.r10.u32;
      d.f1 = ctx.f1.f64;
    }
  }
}

template <uint32_t kAddress>
inline void Leave(PPCContext& ctx, uint32_t detail_index) {
  constexpr size_t i = IndexOf(kAddress);
  if (detail_index < kMaxDetail) {
    g_slots[i].detail[detail_index].ret = ctx.r3.u32;
    g_slots[i].detail[detail_index].done.store(true, std::memory_order_release);
  }
  if constexpr (kAddress == kFrameFunction) {
    OnFrame();
  }
}

}  // namespace

#define X(a)                                   \
  REX_EXTERN(__imp__sub_##a);                  \
  extern "C" REX_FUNC(sub_##a) {               \
    if (!Enabled()) [[likely]] {               \
      __imp__sub_##a(ctx, base);               \
      return;                                  \
    }                                          \
    uint32_t detail_index;                     \
    Enter<0x##a>(ctx, detail_index);           \
    __imp__sub_##a(ctx, base);                 \
    Leave<0x##a>(ctx, detail_index);           \
  }
#include "sf4_d3d_trace_all.inc"
#undef X
