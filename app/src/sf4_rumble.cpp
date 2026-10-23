// SF4 - controller vibration on by default.
//
// WHY
//   SF4 computes rumble on every hit, KO and throw (input manager sub_820CA8D0 stores the two motor values per
//   player) but sends it to the pad only for a player whose "vibration" flag is set (sub_820CA9F0, the input
//   manager's update). That flag comes from the player's button configuration (sub_821E16D8 applies it with
//   SetVibrationEnabled, sub_820CA848), and SF4's default configuration (sub_821E10B0) has it off: a new save never
//   rumbles until the player turns it on in the button configuration. The console's first native run (2026-10-22)
//   showed exactly that: the Joy-Con vibration devices were set up and only ever got zero.
//
// WHAT IT DOES
//   With sf4_vibration_always (the default), SetVibrationEnabled always enables vibration: rumble works with any
//   save. false = the in-game button configuration decides, as on the Xbox 360.
//   The SDK side is input_rumble (sf4.toml / sf4_app.h): without it the SDK sends the pads zero vibration.
//
// HITS LANDED (sf4_vibration_hit_landed)
//   SF4 only rumbles the player who is hit: each fighter's rumble update (sub_820FDC78, once per game step per
//   fighter) plays an envelope set by the hit code (start/end strength at fighter+21444/21448, elapsed/duration
//   floats at +21452/+21456) on that side's controller (battle manager sub_82116638, +200 = controller of a side,
//   controller +24 = rumble, which ends in the input manager's SetMotors), and only for a human side. Landing a hit
//   gave nothing. Here, after the game's own update of a fighter: when its health (int16 at fighter+20948; 900 -> 860
//   -> 820 for Dhalsim's hits taken on the PC) drops, the other side gets a short, lighter rumble that fades out
//   over sf4_hit_landed_steps game steps, stronger for more damage, sent through the same controller path (so the
//   vibration flags and a CPU side, whose controller does not rumble, behave as the game's own). It is skipped while
//   that side's own "hit taken" rumble plays.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <cstring>

REXCVAR_DEFINE_BOOL(sf4_vibration_always, true, "SF4",
                    "Controller vibration on for both players whatever the in-game button configuration says (SF4's "
                    "default configuration has it off). false = the in-game setting decides");

// sub_820CA848(input manager, player 0-1, on): stores `on` at [manager + 44 + 12 * player]; turning it off stops a
// running rumble first.
REX_EXTERN(__imp__sub_820CA848);
REX_HOOK_RAW(sub_820CA848) {
  if (REXCVAR_GET(sf4_vibration_always) && ctx.r5.u32 == 0) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 2) {
      REXLOG_INFO("[sf4] vibration: player {} has it off in the button configuration, on (sf4_vibration_always)",
                  ctx.r4.s32);
    }
    ctx.r5.u64 = 1;
  }
  __imp__sub_820CA848(ctx, base);
}

REXCVAR_DEFINE_INT32(sf4_vibration_hit_landed, 60, "SF4",
                     "Rumble when your hits land, in percent of the strength of the game's own rumble when you are hit "
                     "(0 = off; SF4 itself only rumbles the player who is hit)")
    .range(0, 100);
REXCVAR_DEFINE_INT32(sf4_hit_landed_steps, 9, "SF4",
                     "Length of the rumble for a hit you land, in game steps (60 per second)")
    .range(1, 60);

namespace {

uint32_t Be32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}
void StoreBe32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}
float BeFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Be32(base, address);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}
int16_t Be16(const uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return int16_t(__builtin_bswap16(v));
}

void CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t offset) {
  ctx.r3.u64 = object;
  rex::runtime::ResolveIndirectFunction(Be32(base, Be32(base, object) + offset))(ctx, base);
}

constexpr uint32_t kHealth = 20948;      // int16, the fighter's health
constexpr uint32_t kRumbleElapsed = 21452, kRumbleDuration = 21456;  // floats, the game's "hit taken" envelope

struct Landed {
  float strength = 0;  // 0..1 at the start
  int steps = 0, left = 0;
};
int16_t g_health[2] = {-1, -1};
Landed g_landed[2];

}  // namespace

REX_EXTERN(__imp__sub_82116638);  // battle manager (singleton)

// A fighter's rumble update, once per game step per fighter.
REX_EXTERN(__imp__sub_820FDC78);
REX_HOOK_RAW(sub_820FDC78) {
  const uint32_t fighter = ctx.r3.u32;
  __imp__sub_820FDC78(ctx, base);
  const int percent = REXCVAR_GET(sf4_vibration_hit_landed);
  if (percent <= 0 || !fighter) return;

  const PPCRegister r3 = ctx.r3;
  const uint32_t r1 = ctx.r1.u32;
  CallVirtual(ctx, base, fighter, 4);  // side 0/1
  const uint32_t side = ctx.r3.u32;
  if (side > 1) {
    ctx.r3 = r3;
    return;
  }
  const uint32_t other = side ^ 1;

  // This fighter's health dropped: the other side landed a hit.
  const int16_t health = Be16(base, fighter + kHealth);
  if (g_health[side] > 0 && health >= 0 && health < g_health[side]) {
    const int damage = g_health[side] - health;
    Landed& l = g_landed[other];
    l.strength = std::clamp(0.45f + float(damage) / 200.0f, 0.45f, 1.0f) * float(percent) / 100.0f;
    l.steps = l.left = REXCVAR_GET(sf4_hit_landed_steps);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 6) {
      REXLOG_INFO("[sf4] vibration: side {} landed a hit ({} damage), rumble {:.0f} %", other, damage,
                  double(l.strength) * 100.0);
    }
  }
  g_health[side] = health;

  // This fighter's own landed-hit rumble, unless its "hit taken" rumble is playing.
  Landed& mine = g_landed[side];
  if (mine.left > 0) {
    const float duration = BeFloat(base, fighter + kRumbleDuration);
    const bool own = duration > 0.0f && BeFloat(base, fighter + kRumbleElapsed) < duration;
    const float level = own ? 0.0f : mine.strength * float(mine.left) / float(mine.steps);
    --mine.left;
    if (!own) {
      __imp__sub_82116638(ctx, base);
      const uint32_t battle = ctx.r3.u32;
      ctx.r4.u64 = side;
      CallVirtual(ctx, base, battle, 200);  // the side's controller
      const uint32_t controller = ctx.r3.u32;
      if (controller) {
        // Two words (left, right motor) on the guest stack, below a frame of our own.
        ctx.r1.u64 = r1 - 128;
        const uint32_t motors = ctx.r1.u32 + 80;
        const uint32_t value = mine.left > 0 ? uint32_t(level * 65535.0f) : 0u;
        StoreBe32(base, motors, value);
        StoreBe32(base, motors + 4, value);
        ctx.r4.u64 = motors;
        CallVirtual(ctx, base, controller, 24);
        ctx.r1.u64 = r1;
      }
    }
  }
  ctx.r3 = r3;
}
