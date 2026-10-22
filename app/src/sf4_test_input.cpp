// sf4 - automated tests on the PC (see sf4_test_input.h). The script pad of nfsmw-nx's nfsmw_prueba_entrada.cpp,
// without its car-specific parts.

#include "sf4_test_input.h"

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

REXCVAR_DEFINE_STRING(sf4_test_buttons, "", "SF4",
                      "Tests: button script of a virtual pad, in seconds from start-up. \"8:start,12:a,20-25:right\" "
                      "presses START at 8 s, A at 12 s and holds the D-pad right from 20 to 25 s. Buttons: a b x y "
                      "start back up down left right lb rb lt rt, and the left stick: stick_up stick_down "
                      "stick_left stick_right")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace sf4::test_input {
namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;
namespace in = rex::input;

constexpr in::DeviceId kScriptPad = in::DeviceId(0x5346345F47554900ull);  // "SF4_GUI"
constexpr double kPress = 0.25;  // seconds of a single press

// X_INPUT_GAMEPAD bits; the triggers go in bits 16 and 17, the left stick in 18-21. The key is the
// XInputGetKeystroke VK_PAD code (ui/virtual_key.h), for the menus that read keystrokes instead of state.
struct Button {
  std::string_view name;
  uint32_t bits;
  uint16_t key;
};
constexpr Button kButtons[] = {
    {"a", 0x1000, 0x5800},        {"b", 0x2000, 0x5801},          {"x", 0x4000, 0x5802},
    {"y", 0x8000, 0x5803},        {"rb", 0x0200, 0x5804},         {"lb", 0x0100, 0x5805},
    {"lt", 1u << 16, 0x5806},     {"rt", 1u << 17, 0x5807},       {"up", 0x0001, 0x5810},
    {"down", 0x0002, 0x5811},     {"left", 0x0004, 0x5812},       {"right", 0x0008, 0x5813},
    {"start", 0x0010, 0x5814},    {"back", 0x0020, 0x5815},       {"stick_up", 1u << 18, 0x5820},
    {"stick_down", 1u << 19, 0x5821}, {"stick_right", 1u << 20, 0x5822}, {"stick_left", 1u << 21, 0x5823},
};

struct Step {
  double start;
  double end;
  uint32_t bits;
};

bool Number(std::string_view text, double& value) {
  const std::string copy(text);
  char* end = nullptr;
  value = std::strtod(copy.c_str(), &end);
  return !copy.empty() && end == copy.c_str() + copy.size() && value >= 0.0;
}

// "T:button" presses briefly; "T1-T2:button" holds. Separated by commas, spaces or semicolons.
std::vector<Step> Parse(std::string_view text) {
  std::vector<Step> steps;
  size_t i = 0;
  const auto separator = [](char c) { return c == ' ' || c == ',' || c == ';'; };
  while (i < text.size()) {
    while (i < text.size() && separator(text[i])) ++i;
    size_t j = i;
    while (j < text.size() && !separator(text[j])) ++j;
    if (j == i) break;
    const std::string_view step = text.substr(i, j - i);
    i = j;
    const size_t colon = step.find(':');
    const std::string_view times = step.substr(0, colon);
    const std::string_view name = colon == std::string_view::npos ? std::string_view() : step.substr(colon + 1);
    const Button* button = nullptr;
    for (const Button& b : kButtons) {
      if (b.name == name) button = &b;
    }
    const size_t dash = times.find('-');
    double start = 0.0, end = 0.0;
    const bool ok = button && Number(times.substr(0, dash), start) &&
                    (dash == std::string_view::npos ? (end = start + kPress, true)
                                                    : (Number(times.substr(dash + 1), end) && end > start));
    if (!ok) {
      REXLOG_WARN("[test] script pad: invalid step \"{}\": ignored", step);
      continue;
    }
    steps.push_back({start, end, button->bits});
  }
  return steps;
}

class ScriptPad final : public in::InputDriver {
 public:
  explicit ScriptPad(std::vector<Step> steps)
      : InputDriver(nullptr, 0), steps_(std::move(steps)), start_(Clock::now()) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<in::DeviceInfo>& out) override {
    in::DeviceInfo info;
    info.id = kScriptPad;
    info.name = "Test script pad";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(in::DeviceId id, in::X_INPUT_STATE* out_state) override {
    if (id != kScriptPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    uint32_t packet = 0;
    const uint32_t bits = Update(packet);
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet;
      out_state->gamepad.buttons = uint16_t(bits & 0xFFFF);
      out_state->gamepad.left_trigger = ((bits >> 16) & 1) ? 0xFF : 0;
      out_state->gamepad.right_trigger = ((bits >> 17) & 1) ? 0xFF : 0;
      out_state->gamepad.thumb_ly = ((bits >> 18) & 1) ? 30000 : ((bits >> 19) & 1) ? -30000 : 0;
      out_state->gamepad.thumb_lx = ((bits >> 20) & 1) ? 30000 : ((bits >> 21) & 1) ? -30000 : 0;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(in::DeviceId id, uint32_t flags, in::X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (id != kScriptPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      // Like the SDK's empty pad (nop_input_driver.cpp).
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(in::DeviceId id, in::X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    return id == kScriptPad ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(in::DeviceId id, uint32_t flags, in::X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    if (id != kScriptPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    uint32_t packet = 0;
    Update(packet);
    std::lock_guard<std::mutex> lock(mutex_);
    if (keys_.empty()) return X_ERROR_EMPTY;
    if (out_keystroke) *out_keystroke = keys_.front();
    keys_.pop_front();
    return X_ERROR_SUCCESS;
  }

 private:
  // Script buttons at this instant. Each change bumps packet_number and queues the pressed and released keys.
  uint32_t Update(uint32_t& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    const double t = std::chrono::duration<double>(Clock::now() - start_).count();
    uint32_t bits = 0;
    for (const Step& s : steps_) {
      if (t >= s.start && t < s.end) bits |= s.bits;
    }
    if (bits != current_) {
      for (const Button& b : kButtons) {
        const bool before = (current_ & b.bits) != 0;
        const bool now = (bits & b.bits) != 0;
        if (before == now) continue;
        in::X_INPUT_KEYSTROKE key{};
        key.virtual_key = b.key;
        key.flags = uint16_t(now ? in::X_INPUT_KEYSTROKE_KEYDOWN : in::X_INPUT_KEYSTROKE_KEYUP);
        if (keys_.size() < 64) keys_.push_back(key);
        REXLOG_INFO("[test] script pad: {} {} at {:.1f} s", b.name, now ? "down" : "up", t);
      }
      current_ = bits;
      ++packet_;
    }
    packet = packet_;
    return bits;
  }

  const std::vector<Step> steps_;
  const Clock::time_point start_;
  std::mutex mutex_;
  uint32_t current_ = 0;
  uint32_t packet_ = 1;
  std::deque<in::X_INPUT_KEYSTROKE> keys_;
};

}  // namespace

void WrapInput(rex::RuntimeConfig& config) {
  const std::string script = REXCVAR_GET(sf4_test_buttons);
  if (script.empty() || !config.input_factory) return;
  std::vector<Step> steps = Parse(script);
  if (steps.empty()) {
    REXLOG_WARN("[test] script pad: \"{}\" has no valid steps", script);
    return;
  }
  auto base = config.input_factory;
  config.input_factory = [base, steps](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto system = base(tool_mode);
    if (system && !tool_mode) {
      // The default factory is CreateDefaultInputSystem (ui/rex_app.cpp), which returns a rex::input::InputSystem.
      static_cast<in::InputSystem*>(system.get())->AddDriver(std::make_unique<ScriptPad>(steps));
      REXLOG_INFO("[test] script pad with {} steps", steps.size());
    }
    return system;
  };
}

}  // namespace sf4::test_input
