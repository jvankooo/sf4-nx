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

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>

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
