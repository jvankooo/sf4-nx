// sf4 - ReXGlue Recompiled Project (Street Fighter IV, Xbox 360, title 434307E1)
//
// Customize your app by overriding virtual hooks from rex::ReXApp.
//
// The game draws through the native renderer from masseffect-nx (src/native, docs/native-renderer.md) on the
// Switch; the SDK's Xenos GPU emulation stays as the fallback (masseffect_renderer_native = false) and is what the
// Direct3D 12 PC build (tools/build_pc.bat) uses.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/overlay/settings_overlay.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "sf4_capture.h"
#include "sf4_test_input.h"
#if SF4_NATIVE_RENDERER
#include "native/me_native_system.h"
#endif

#if REX_PLATFORM_SWITCH
extern "C" {
void RexSwitchApmRequestGpuMhz(int mhz);  // src/ui/switch_apm.cpp of the SDK
void RexSwitchApmApply(void);
const char* RexSwitchLogDir(void);  // src/ui/switch_crash_hooks.c of the SDK
}
REXCVAR_DECLARE(bool, sf4_stack_sampling);  // src/sf4_jobs.cpp
#endif

class Sf4App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Sf4App>(new Sf4App(ctx, "sf4",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Automated tests: virtual pad if sf4_test_buttons has a script.
    sf4::test_input::WrapInput(config);
#if REX_PLATFORM_SWITCH
    // Nintendo's official 460.8 MHz handheld GPU profile instead of the 307.2 MHz default (+50 % GPU), as
    // masseffect-nx does: a standard performance configuration of the console, not an overclock. The console log
    // of 2026-10-22 showed the game waiting on the GPU most of the time (rex_profile.log, "fences waited").
    RexSwitchApmRequestGpuMhz(460);
    RexSwitchApmApply();
#endif
#if SF4_NATIVE_RENDERER
    // The native renderer from masseffect-nx (src/native) replaces the Xenos emulation plugin when
    // masseffect_renderer_native = true.
    if (me::native::Enabled()) {
      config.graphics = me::native::CreateGraphicsSystem();
    }
#endif
  }

 protected:
  // Saves next to the executable: sdmc:/switch/sf4-nx/saves on the Switch.
  // Runs before sf4.toml is read, so the toml can still change it.
  void ElegirCarpetaDeGuardados() {
    if (rex::cvar::GetFlagInfo("content_backup_root") == nullptr) {
      return;
    }
    if (!rex::cvar::GetFlagByName("content_backup_root").empty()) {
      return;
    }
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty()) {
      return;
    }
    rex::cvar::SetFlagByName("content_backup_root", rex::path_to_utf8(carpeta / "saves"));
  }

  // Game files next to the executable, so the NRO starts without arguments:
  //   1. game_root/ (the extracted disc, what the Switch package uses)
  //   2. an .iso whose name matches the executable's, then any other .iso
  // --game_data_root on the command line takes precedence.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    ElegirCarpetaDeGuardados();
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty() || !std::filesystem::is_directory(carpeta, ec)) {
      return;
    }
    const auto extraida = carpeta / "game_root";
    if (std::filesystem::is_regular_file(extraida / "default.xex", ec)) {
      paths.game_data_root = extraida;
      return;
    }

    auto minusculas = [](std::string s) {
      std::transform(s.begin(), s.end(), s.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      return s;
    };
    std::string yo;
    if (const auto exe = rex::filesystem::GetExecutablePath(); !exe.empty()) {
      yo = minusculas(exe.stem().string());
    }
    std::filesystem::path preferida;
    std::vector<std::filesystem::path> otras;
    for (const auto& e : std::filesystem::directory_iterator(carpeta, ec)) {
      if (ec) break;
      if (!e.is_regular_file(ec) || minusculas(e.path().extension().string()) != ".iso") continue;
      if (!yo.empty() && minusculas(e.path().stem().string()) == yo) {
        preferida = e.path();
      } else {
        otras.push_back(e.path());
      }
    }
    if (!preferida.empty()) {
      paths.game_data_root = preferida;
    } else if (!otras.empty()) {
      std::sort(otras.begin(), otras.end());
      paths.game_data_root = otras.front();
    }
    // Nothing found: left empty so the SDK prints its own message.
  }

  // Analysis aid (as masseffect-nx): with SF4_DUMP_IMAGE=<file>, write the loaded guest image
  // (image_base..image_base+image_size) so tools/pointer_scan.py can look for code pointers stored in data.
  // Does nothing unless the variable is set.
  void OnPostLoadXexImage() override {
    const char* path = std::getenv("SF4_DUMP_IMAGE");
    if (!path || !runtime() || !runtime()->memory()) {
      return;
    }
    const uint8_t* image =
        runtime()->memory()->TranslateVirtual<const uint8_t*>(PPCImageConfig.image_base);
    if (std::FILE* f = std::fopen(path, "wb")) {
      std::fwrite(image, 1, PPCImageConfig.image_size, f);
      std::fclose(f);
    }
  }

  // The native renderer's settings (categories "Mass Effect" and "MASSEFFECT", names masseffect_*: the renderer
  // comes from masseffect-nx) are left out of the settings menu. They still work from sf4.toml and the command
  // line; in the menu they were about 350 entries named after another game, most only read at startup.
  static void HideMassEffectSettings() {
    std::vector<std::string> names;
    for (const auto& entry : rex::cvar::GetRegistry()) {
      std::string category = entry.category;
      category.erase(std::remove(category.begin(), category.end(), ' '), category.end());
      std::transform(category.begin(), category.end(), category.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (category.find("masseffect") != std::string::npos || entry.name.rfind("masseffect", 0) == 0)
        names.push_back(entry.name);
    }
    rex::ui::HideSettingsInMenu(names);
    REXLOG_INFO("[sf4] settings menu: {} native renderer settings (masseffect) hidden", names.size());
  }

  void OnPostInitLogging() override {
    HideMassEffectSettings();
#if REX_PLATFORM_SWITCH
    // sf4_stack_sampling: the SDK profiler samples the threads' stacks (logs/rex/rex_profile.log, where each thread
    // spends its time) when logs/rex/stacks_profile.flag exists 8 s after start-up; this writes that file.
    // false removes the file, which a sampling run leaves behind (sampling costs speed).
    {
      const std::string flag = std::string(RexSwitchLogDir()) + "stacks_profile.flag";
      if (REXCVAR_GET(sf4_stack_sampling)) {
        if (std::FILE* f = std::fopen(flag.c_str(), "w")) {
          std::fputs("written by sf4_stack_sampling = true (sf4.toml)\n", f);
          std::fclose(f);
        }
      } else {
        std::remove(flag.c_str());
      }
    }
#endif
    // Without a GPU plugin the screen stays black: the game runs, but the runtime discards its
    // graphics calls with "no GPU emulation loaded".
    PonerSiNadieLoPidio("gpu_plugin", "xenos");
    // SF4 clears or rewrites its depth buffers before testing against them: skip the depth / stencil EDRAM copies,
    // nine draws each on the Switch (sf4.toml). The color copies stay: the fight HUD portraits need them.
    PonerSiNadieLoPidio("render_target_depth_transfers", "false");
    // The game's vibration reaches the pads (the SDK sends them zero otherwise); see src/sf4_rumble.cpp for the
    // game side.
    PonerSiNadieLoPidio("input_rumble", "true");
#if !REX_PLATFORM_SWITCH
    PonerSiNadieLoPidio("mnk_mode", "true");
#else
#if SF4_NATIVE_RENDERER
    // 720p, the handheld screen's own size, with the native renderer. SF4 sizes its scene, depth and
    // post-processing buffers from the video mode the console reports; sf4.toml can lower it (854x480: a 640x480
    // scene).
    // The native renderer (docs/native-renderer.md). EDRAM mode 0: one image per render target and no transfers
    // between views; SF4 clears or redraws every target before reading it, and the views Direct3D clears through
    // are redrawn on their surface (sf4_native_msaa4_views). The other modes leave a seam at 720p.
    PonerSiNadieLoPidio("masseffect_renderer_native", "true");
    PonerSiNadieLoPidio("masseffect_native_edram_alias_mode", "0");
    // Mass Effect's own shortcuts, keyed on its passes: off for SF4.
    PonerSiNadieLoPidio("masseffect_native_skip_prepass", "false");
    PonerSiNadieLoPidio("masseffect_native_postponed_sky", "false");
#else
    // 480p with the Xenos emulation, which shades about 0.3 Gpixel/s on the Switch (console run 4: GPU busy 93 % at
    // 720p, 4 fps): at 854x480 the viewports are 854x480 and the full-size buffers 640x480.
    PonerSiNadieLoPidio("video_mode_width", "854");
    PonerSiNadieLoPidio("video_mode_height", "480");
#endif
#endif
  }

  void OnPostSetup() override {
    // readback_resolve stays at the SDK default, "none". It was forced to "fast" here, copied from nfsmw-nx (whose
    // exposure is measured on the CPU): that copies every resolve back into guest memory each frame, which
    // invalidates the textures there and makes them load again. On the console (2026-10-22) it went with the
    // character select at 0.9 fps: 15 resolves and 11 texture loads per frame, the game waiting on the GPU 90 % of
    // the time. SF4 does not read its resolves on the CPU (a whole arcade round plays the same on the PC with "none").

    // PNG captures for scripted tests (sf4_capture_every_s; off by default).
    sf4::capture::Start([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
  }

  void OnShutdown() override { sf4::capture::Stop(); }

 private:
  // Only touches settings nobody set explicitly (command line or sf4.toml win).
  static void PonerSiNadieLoPidio(const char* nombre, const char* valor) {
    if (rex::cvar::GetFlagInfo(nombre) == nullptr) {
      REXLOG_DEBUG("Ajuste '{}' no registrado todavia; no lo toco.", nombre);
      return;
    }
    if (rex::cvar::HasNonDefaultValue(nombre)) {
      return;
    }
    if (rex::cvar::SetFlagByName(nombre, valor)) {
      REXLOG_DEBUG("Ajuste por defecto: {} = {}", nombre, valor);
    }
  }
};
