// sf4 - readable dumps of whole frames of the game's command stream (native renderer, step 2 of docs/d3d-map.md).
//
// With sf4_frame_dump = "75,90" the buffers the game plays at 75 s and at 90 s after the player's first call (and
// the sf4_frame_dump_count - 1 buffers after each) are decoded, before the original plays them, into
// frames/frame_<seconds>s_<n>.txt next to the executable:
//
//   - events in order: render targets and depth buffer, viewport, clears, resolves, tiling, constant loads...
//   - every draw on one line: primitive type, primitive and vertex/index counts, then the state that changed since
//     the previous draw (all of it at the first draw after a render target change): shaders, vertex declaration,
//     vertex and index buffers, textures with their size and format and their sampler states, depth, stencil, alpha,
//     blend, color mask, cull, depth bias, clip planes, predication, and the shader constant ranges written since
//     the previous draw
//   - a summary: commands per opcode, draws per pass
//
// Resources are shown by their type and guest address ("VS@409D2B70"), so the same texture or shader can be
// followed through the frame and from one frame to the next.
// Off by default; when off, the only cost is one flag test per frame in the player hook (sf4_commands.cpp).

#include "sf4_commands.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(sf4_frame_dump, "", "SF4",
                      "Seconds after the first frame at which the game's command stream is decoded into "
                      "frames/frame_<s>s_<n>.txt, separated by commas (\"75,90\"); empty = never")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sf4_frame_dump_count, 1, "SF4", "Consecutive frames decoded at each sf4_frame_dump time")
    .range(1, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace sf4::frame_decode {
namespace {

namespace cmd = sf4::commands;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kPrimitiveCountTable = 0x8206F660;  // per type: count = primitives * [0] + [1] (op 28-31 code)

std::string Format(const char* fmt, ...) {
  char b[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(b, sizeof(b), fmt, args);
  va_end(args);
  return b;
}

const char* Compare(uint32_t f) {
  static const char* kNames[] = {"never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"};
  return f < 8 ? kNames[f] : "?";
}

const char* Primitive(uint32_t t) {
  switch (t) {
    case 1: return "points";
    case 2: return "lines";
    case 3: return "line-strip";
    case 4: return "triangles";
    case 5: return "fan";
    case 6: return "strip";
    case 8: return "rects";
    case 13: return "quads";
    default: return "type?";
  }
}

uint32_t VertexCount(uint32_t type, uint32_t primitives) {
  const uint32_t entry = kPrimitiveCountTable + 8 * type;
  if (type > 31 || !cmd::Readable(entry, 8)) return 0;
  return cmd::Word(entry) * primitives + cmd::Word(entry + 4);
}

// "VS@409D2B70": the resource type of the XDK header (low nibble of its first word) and the object's guest address,
// which is unique and stays the same for an object (the third header word, tried first, is shared by many objects:
// it changes with the frame, like a fence).
std::string Resource(uint32_t address) {
  if (!address) return "none";
  if (!cmd::Readable(address, 4)) return Format("?@%08X", address);
  static const char* kTypes[] = {"res", "VB", "IB", "tex", "surf", "decl", "VS", "PS", "CB"};
  const uint32_t type = cmd::Word(address) & 0xF;
  return Format("%s@%08X", type < 9 ? kTypes[type] : "res", address);
}

// A texture with the size and format of its fetch constant (6 words at +28, which SetTexture copies into the slot).
std::string Texture(uint32_t address) {
  std::string s = Resource(address);
  if (!address || !cmd::Readable(address + 28, 24)) return s;
  const uint32_t f1 = cmd::Word(address + 32), f2 = cmd::Word(address + 36), f5 = cmd::Word(address + 48);
  const uint32_t dimension = (f5 >> 9) & 3;
  if (dimension == 1) {  // 2D
    s += Format(" %ux%u", (f2 & 0x1FFF) + 1, ((f2 >> 13) & 0x1FFF) + 1);
  } else {
    s += Format(" dim%u", dimension);
  }
  s += Format(" fmt%02X", f1 & 0x3F);
  return s;
}

// The surface words after the header are not decoded yet: printed raw for the next step.
std::string Surface(uint32_t address) {
  std::string s = Resource(address);
  if (address && cmd::Readable(address + 24, 16)) {
    s += Format(" [%08X %08X %08X %08X]", cmd::Word(address + 24), cmd::Word(address + 28), cmd::Word(address + 32),
                cmd::Word(address + 36));
  }
  return s;
}

struct Decoder {
  FILE* f = nullptr;
  // Render state, as strings per item: printed when they change.
  std::map<std::string, std::string> state, printed;
  uint32_t texture[32]{};
  std::string address_mode[32], filter[32], anisotropy[32];  // sampler states, printed in this order
  uint32_t z_enable = 0, z_write = 0, z_func = 0, stencil_enable = 0, stencil_func = 0, stencil_ref = 0,
           stencil_mask = 0, stencil_write = 0, stencil_fail = 0, stencil_zfail = 0, stencil_pass = 0,
           alpha_test = 0, alpha_func = 7, alpha_ref = 0;
  std::string vs_constants, ps_constants;
  std::vector<cmd::DrawCall> expected, actual;  // draw function calls: predicted from the commands, then seen
  int draws = 0, pass = 0, pass_draws = 0;
  std::vector<int> pass_sizes;
  uint64_t counts[cmd::kOpcodes]{};

  void Line(const std::string& s) { std::fprintf(f, "%s\n", s.c_str()); }

  void NewPass(const std::string& why) {
    if (pass_draws || pass) pass_sizes.push_back(pass_draws);
    ++pass;
    pass_draws = 0;
    printed.clear();  // the first draw of a pass prints the whole state
    Line(Format("== pass %d: %s", pass, why.c_str()));
  }

  void Constants(std::string& list, uint32_t command) {
    const uint32_t w1 = cmd::Word(command + 4);
    const uint32_t start = w1 >> 16, count = w1 & 0xFFFF;
    if (!list.empty()) list += ',';
    list += count > 1 ? Format("c%u-%u", start, start + count - 1) : Format("c%u", start);
  }

  void Draw(uint32_t op, uint32_t command) {
    const uint32_t type = cmd::Word(command + 4), primitives = cmd::Word(command + 8);
    std::string head = Format("draw %d  %-22s %-9s prims %u, %s %u", draws, cmd::OpName(op), Primitive(type),
                              primitives, op == 29 || op == 31 ? "indices" : "vertices", VertexCount(type, primitives));
    // What the handler will pass to the draw function (checked after the frame is played, see Verify).
    const uint32_t count = VertexCount(type, primitives);
    if (op == 28) {
      head += Format(", start vertex %u", cmd::Word(command + 12));
      expected.push_back({cmd::kDrawVertices, type, cmd::Word(command + 12), 0, count});
    }
    if (op == 29) {
      // D3D9's DrawIndexedPrimitive order: w3 base vertex, w4 number of vertices (not passed on), w5 start index.
      head += Format(", base vertex %u, start index %u, %u vertices", cmd::Word(command + 12),
                     cmd::Word(command + 20), cmd::Word(command + 16));
      expected.push_back({cmd::kDrawIndexedVertices, type, cmd::Word(command + 12), cmd::Word(command + 20), count});
    }
    if (op == 30) {
      head += Format(", stride %u, inline data", cmd::Word(command + 12));
      expected.push_back({cmd::kBeginVertices, type, 0, cmd::Word(command + 12), count});
    }
    if (op == 31) expected.push_back({cmd::kBeginIndexedVertices, type, 0, 0, count});
    ++draws;
    ++pass_draws;
    // State items.
    state["z"] = z_enable ? Format("%s%s", Compare(z_func), z_write ? " write" : "") : "off";
    state["stencil"] = stencil_enable
                           ? Format("%s ref %02X mask %02X write %02X ops %u/%u/%u", Compare(stencil_func),
                                    stencil_ref, stencil_mask & 0xFF, stencil_write & 0xFF, stencil_fail,
                                    stencil_zfail, stencil_pass)
                           : "off";
    state["alpha"] = alpha_test ? Format("%s %u", Compare(alpha_func), alpha_ref) : "off";
    for (int s = 0; s < 32; ++s) {
      const std::string key = Format("t%d", s);
      if (texture[s]) {
        std::string text = Texture(texture[s]);
        for (const std::string* part : {&address_mode[s], &filter[s], &anisotropy[s]}) {
          if (!part->empty()) text += " " + *part;
        }
        state[key] = text;
      } else {
        state.erase(key);
      }
    }
    std::string changed;
    for (const auto& [key, value] : state) {
      auto it = printed.find(key);
      if (it == printed.end() || it->second != value) changed += "  " + key + "=" + value;
    }
    for (auto it = printed.begin(); it != printed.end();) {
      if (!state.count(it->first)) {
        changed += "  " + it->first + "=none";
        it = printed.erase(it);
      } else {
        ++it;
      }
    }
    printed = state;
    Line(head);
    if (!changed.empty()) Line("      " + changed);
    if (!vs_constants.empty() || !ps_constants.empty()) {
      Line("        constants:" + (vs_constants.empty() ? "" : " vs " + vs_constants) +
           (ps_constants.empty() ? "" : " ps " + ps_constants));
      vs_constants.clear();
      ps_constants.clear();
    }
  }

  void Command(uint32_t op, uint32_t c, uint32_t length) {
    ++counts[op];
    auto w = [&](uint32_t k) { return k < length ? cmd::Word(c + 4 * k) : 0u; };
    auto fl = [&](uint32_t k) { return k < length ? cmd::Float(c + 4 * k) : 0.0f; };
    switch (op) {
      case 3: Line(Format("callback %u", w(1))); break;
      case 4:
        NewPass(Format("render target %u = %s", w(1), w(2) ? Surface(w(2)).c_str() : "none"));
        state[Format("rt%u", w(1))] = Resource(w(2));
        if (!w(2)) state.erase(Format("rt%u", w(1)));
        break;
      case 5:
        Line(Format("depth buffer = %s", w(1) ? Surface(w(1)).c_str() : "none"));
        state["depth"] = Resource(w(1));
        break;
      case 7:
        NewPass("default render target and depth buffer");
        state.erase("rt0");
        state["depth"] = "default";
        break;
      case 8:
        Line(Format("clear flags %02X color (%g %g %g %g) z %g stencil %u", w(1), fl(2), fl(3), fl(4), fl(5), fl(6),
                    w(7)));
        break;
      case 9:
        Line(Format("viewport %u,%u %ux%u z %g-%g", w(1) >> 16, w(1) & 0xFFFF, w(2) >> 16, w(2) & 0xFFFF, fl(3),
                    fl(4)));
        break;
      case 10: state[Format("mask%u", w(1))] = Format("%X", w(2)); break;
      case 11: z_enable = w(1); break;
      case 12: z_write = w(1); break;
      case 13: z_func = w(1); break;
      case 14: alpha_test = w(1); break;
      case 15: alpha_func = w(1), alpha_ref = w(2); break;
      case 16: stencil_enable = w(1); break;
      case 17: stencil_func = w(1), stencil_ref = w(2), stencil_mask = w(3); break;
      case 18: stencil_write = w(1); break;
      case 19: stencil_fail = w(1), stencil_zfail = w(2), stencil_pass = w(3); break;
      case 20: state["fill"] = Format("%u", (w(1)) & 0xFF); break;
      case 21: state["cull"] = Format("%u", w(1)); break;
      case 22: state["bias"] = Format("%g/%g", fl(1), fl(2)); break;
      case 23: state[Format("blend%u", w(1))] = Format("%08X", w(2)); break;
      case 25:  // bytes U, V, W
        address_mode[w(1) & 31] = Format("addr %u/%u/%u", w(2) >> 24, (w(2) >> 16) & 0xFF, (w(2) >> 8) & 0xFF);
        break;
      case 26: {  // bytes mag, min, mip, anisotropic
        const uint32_t v = w(2);
        filter[w(1) & 31] = Format("filt %u/%u/%u", v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF) +
                            ((v & 0xFF) ? Format("/%u", v & 0xFF) : std::string());
        break;
      }
      case 27: anisotropy[w(1) & 31] = Format("aniso %u", w(2)); break;
      case 28:
      case 29:
      case 30:
      case 31: Draw(op, c); break;
      case 32: state["vb"] = Format("%s +%u stride %u", Resource(w(3)).c_str(), w(1), w(2)); break;
      case 33: state["ib"] = Resource(w(1)); break;
      case 34: state["decl"] = Resource(w(1)); break;
      case 35: state["vs"] = Resource(w(1)); break;
      case 36: state["ps"] = Resource(w(1)); break;
      case 37: Constants(vs_constants, c); break;
      case 38: vs_constants += (vs_constants.empty() ? "" : ",") + Format("i%u", w(1) >> 16); break;
      case 39: vs_constants += (vs_constants.empty() ? "" : ",") + Format("b%u", w(1) >> 16); break;
      case 40: Constants(ps_constants, c); break;
      case 41: ps_constants += (ps_constants.empty() ? "" : ",") + Format("i%u", w(1) >> 16); break;
      case 42: ps_constants += (ps_constants.empty() ? "" : ",") + Format("b%u", w(1) >> 16); break;
      case 43: texture[w(1) & 31] = w(2); break;
      case 44: state["clip"] = Format("%X", w(1)); break;
      case 45: Line(Format("clip plane %u (%g %g %g %g)", w(1), fl(2), fl(3), fl(4), fl(5))); break;
      case 50:
        Line(Format("resolve flags %X to %s level %u slice %u", w(1), Texture(w(3)).c_str(), w(5), w(6)));
        break;
      case 51: NewPass(Format("begin tiling: %u tiles, flags %X", w(2), w(1))); break;
      case 52: Line(Format("end tiling, flags %X, destination %s", w(1), Resource(w(3)).c_str())); break;
      case 53: state["pred"] = Format("%X", w(1)); break;
      case 54: Line(Format("shader registers: vertex %u, pixel %u", w(2), w(3))); break;
      case 55:
      case 56: Line(Format("%s word %u bits %u-%u", cmd::OpName(op), w(1) >> 16, w(2), w(3))); break;
      case 57:
      case 58:
        Line(Format("%s c%u-%u from %08X", cmd::OpName(op), w(1) >> 16, (w(1) >> 16) + (w(1) & 0xFFFF) - 1, w(2)));
        break;
      default: break;  // 2 (inline data), 24 and the unused ones
    }
  }
};

// The decoded frame being played: its draw function calls are collected and compared with the prediction.
std::unique_ptr<Decoder> g_pending;

// After the frame is played: the draw functions the handlers called against the draws the decoder predicted from
// the commands, in order. A mismatch means a payload word is misread in docs/d3d-map.md or in Draw above.
void Verify(Decoder& d) {
  static const char* kFunctions[] = {"DrawVertices", "DrawIndexedVertices", "BeginVertices", "BeginIndexedVertices"};
  auto text = [](const cmd::DrawCall& c) {
    return Format("%s type %u, %u, %u, count %u", kFunctions[c.function], c.type, c.first, c.second, c.count);
  };
  int mismatches = 0;
  const size_t n = std::max(d.expected.size(), d.actual.size());
  for (size_t i = 0; i < n; ++i) {
    const bool have_e = i < d.expected.size(), have_a = i < d.actual.size();
    const bool same = have_e && have_a && d.expected[i].function == d.actual[i].function &&
                      d.expected[i].type == d.actual[i].type && d.expected[i].first == d.actual[i].first &&
                      d.expected[i].second == d.actual[i].second && d.expected[i].count == d.actual[i].count;
    if (same) continue;
    if (++mismatches <= 20) {
      std::fprintf(d.f, "  draw %zu: decoded %s, played %s\n", i, have_e ? text(d.expected[i]).c_str() : "nothing",
                   have_a ? text(d.actual[i]).c_str() : "nothing");
    }
  }
  std::fprintf(d.f, "== verification: %zu draws decoded, %zu draw function calls while it played, %d mismatch(es)\n",
               d.expected.size(), d.actual.size(), mismatches);
  REXLOG_INFO("[frame] verification: {} decoded, {} played, {} mismatch(es)", d.expected.size(), d.actual.size(),
              mismatches);
}

std::mutex g_mutex;
std::vector<double> g_times;  // seconds still to dump, ascending
int g_count = 1;
bool g_started = false;
Clock::time_point g_start;
int g_left_at_time = 0;  // frames still to dump at the current time
double g_current_time = 0;
int g_frame_in_time = 0;

void DumpFrame(uint32_t buffer, double seconds, int index) {
  const auto folder = rex::filesystem::GetExecutableFolder() / "frames";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const auto path = folder / Format("frame_%04ds_%02d.txt", int(g_current_time), index);
  g_pending = std::make_unique<Decoder>();
  Decoder& d = *g_pending;
  d.f = std::fopen(rex::path_to_utf8(path).c_str(), "w");
  if (!d.f) {
    g_pending.reset();
    return;
  }
  std::fprintf(d.f, "SF4 command stream, buffer %08X, %.2f s after the first frame\n", buffer, seconds);
  int commands = 0;
  const bool whole = cmd::ForEachCommand(buffer, [&](uint32_t op, uint32_t c, uint32_t length) {
    ++commands;
    d.Command(op, c, length);
  });
  d.pass_sizes.push_back(d.pass_draws);
  std::fprintf(d.f, "\n== summary: %d commands%s, %d draws in %d passes\n", commands,
               whole ? "" : " (buffer ended early: unreadable or malformed)", d.draws, d.pass);
  std::string sizes;
  for (size_t i = 0; i < d.pass_sizes.size(); ++i) sizes += Format(" %d", d.pass_sizes[i]);
  std::fprintf(d.f, "draws per pass:%s\n", sizes.c_str());
  for (uint32_t op = 0; op < cmd::kOpcodes; ++op) {
    if (d.counts[op]) std::fprintf(d.f, "  op %2u %-28s %6llu\n", op, cmd::OpName(op), (unsigned long long)d.counts[op]);
  }
  REXLOG_INFO("[frame] {}: {} commands, {} draws", rex::path_to_utf8(path), commands, d.draws);
  // The file stays open: AfterPlay appends the verification once the original has played the buffer.
}

void AfterPlay(uint32_t buffer) {
  (void)buffer;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_pending) return;
  Verify(*g_pending);
  std::fclose(g_pending->f);
  g_pending.reset();
}

void OnDraw(const cmd::DrawCall& call) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pending) g_pending->actual.push_back(call);
}

void OnPlay(uint32_t buffer) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_started) {
    g_started = true;
    g_start = Clock::now();
  }
  const double t = std::chrono::duration<double>(Clock::now() - g_start).count();
  if (!g_left_at_time) {
    if (g_times.empty() || t < g_times.front()) return;
    g_current_time = g_times.front();
    g_times.erase(g_times.begin());
    g_left_at_time = g_count;
    g_frame_in_time = 0;
  }
  DumpFrame(buffer, t, g_frame_in_time++);
  --g_left_at_time;
}

// Registers the observer from a static constructor of this file. The cvars are not loaded yet at that point, so the
// observer reads them at the first frame.
struct Init {
  Init() {
    sf4::commands::AddPlayerObserver([](uint32_t buffer) {
      static int state = 0;  // 0 not read yet, 1 off, 2 on
      if (state == 0) {
        const std::string text = REXCVAR_GET(sf4_frame_dump);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_count = REXCVAR_GET(sf4_frame_dump_count);
        size_t i = 0;
        while (i < text.size()) {
          const size_t comma = text.find(',', i);
          const std::string part = text.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
          if (!part.empty()) g_times.push_back(std::atof(part.c_str()));
          if (comma == std::string::npos) break;
          i = comma + 1;
        }
        std::sort(g_times.begin(), g_times.end());
        state = g_times.empty() ? 1 : 2;
        if (state == 2) {
          REXLOG_INFO("[frame] decoding {} time(s) x {} frame(s) to frames/", g_times.size(), g_count);
          sf4::commands::AddAfterPlayObserver(&AfterPlay);
          sf4::commands::AddDrawObserver(&OnDraw);
        }
      }
      if (state == 2) OnPlay(buffer);
    });
  }
} g_init;

}  // namespace
}  // namespace sf4::frame_decode
