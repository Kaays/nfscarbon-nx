// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: the app (ReXApp hooks).
//
// Based on nfs-carbon-recomp/src/nfscarbon_app.h (Sampreeth004) with the
// Switch parts of nfsmw-nx/app/src/nfsmw_app.h (StevensND): where the game
// files and saves are found next to the NRO, and the native renderer installed
// as config.graphics, because an NRO cannot load GPU plugins at run time.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/rex_app.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "nfscarbon_switch.h"

class NfsCarbonApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NfsCarbonApp>(new NfsCarbonApp(ctx, "nfscarbon", PPCImageConfig));
  }

  // --------------------------------------------------------------------------
  //  Game files
  //
  //  Runs before nfscarbon.toml is read, so --game_data_root on the command
  //  line wins and otherwise the first match next to the executable:
  //    1. game_root/ (the extracted disc: default.xex, NFS/, ...)
  //    2. game/      (the folder name nfs-carbon-recomp uses on PC)
  //    3. an .iso whose name matches the executable, then any other .iso
  //  On the Switch the NRO lives in sdmc:/switch/nfscarbon-nx/.
  // --------------------------------------------------------------------------
  void OnConfigurePaths(rex::PathConfig& paths) override {
    ChooseSaveBackupFolder();
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
    std::vector<std::filesystem::path> folders = {exe_dir / "game_root", exe_dir / "game"};
#if !REX_PLATFORM_SWITCH
    folders.push_back(std::filesystem::current_path(ec) / "game");
    folders.push_back((exe_dir / ".." / ".." / ".." / "game").lexically_normal());
#endif
    for (const auto& folder : folders) {
      if (std::filesystem::is_directory(folder / "NFS", ec) ||
          std::filesystem::is_regular_file(folder / "default.xex", ec)) {
        paths.game_data_root = folder;
        return;
      }
    }
    if (exe_dir.empty() || !std::filesystem::is_directory(exe_dir, ec)) {
      return;
    }
    const std::string me = Lower(rex::filesystem::GetExecutablePath().stem().string());
    std::vector<std::filesystem::path> isos;
    for (const auto& entry : std::filesystem::directory_iterator(exe_dir, ec)) {
      if (ec) break;
      if (!entry.is_regular_file(ec) || Lower(entry.path().extension().string()) != ".iso") continue;
      if (!me.empty() && Lower(entry.path().stem().string()) == me) {
        paths.game_data_root = entry.path();
        return;
      }
      isos.push_back(entry.path());
    }
    if (!isos.empty()) {
      std::sort(isos.begin(), isos.end());
      paths.game_data_root = isos.front();
    }
    // Nothing found: left empty, the SDK reports it.
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (nfscarbon::switch_port::NativeRendererActive()) {
#if defined(NFSCARBON_STATIC_RENDERER)
      // Switch: linked in, created directly. ReXApp only loads a GPU plugin
      // when config.graphics is empty.
      config.graphics = nfscarbon::switch_port::CreateNativeRenderer();
#else
      if (config.gpu_plugin.empty()) {
        config.gpu_plugin = "carbon";
      }
#endif
    } else if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }

  void OnPostInitLogging() override {
    // The toml has been read; Vulkan is created later, in SetupPresentation.
    nfscarbon::switch_port::ApplyEarlySettings();
    if (!nfscarbon::switch_port::NativeRendererActive()) {
      SetIfDefault("gpu_plugin", "xenos");
    }
#if !REX_PLATFORM_SWITCH
    // Keyboard and mouse besides the gamepad; the Switch has neither.
    SetIfDefault("mnk_mode", "true");
#endif
  }

  void OnPostSetup() override {
    // Registered by the Xenos plugin; harmless (unregistered) with the native renderer.
    SetIfDefault("gpu_allow_invalid_fetch_constants", "true");
  }

 private:
  static std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return text;
  }

  // Only settings nobody set (command line or toml) are touched.
  static void SetIfDefault(const char* name, const char* value) {
    if (rex::cvar::GetFlagInfo(name) == nullptr || rex::cvar::HasNonDefaultValue(name)) {
      return;
    }
    rex::cvar::SetFlagByName(name, value);
  }

  // A copy of each save, sorted by profile, next to the executable
  // (saves/<profile>/actual and saves/<profile>/anterior), as in nfsmw-nx.
  static void ChooseSaveBackupFolder() {
    if (rex::cvar::GetFlagInfo("content_backup_root") == nullptr ||
        !rex::cvar::GetFlagByName("content_backup_root").empty()) {
      return;
    }
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (!exe_dir.empty()) {
      rex::cvar::SetFlagByName("content_backup_root", rex::path_to_utf8(exe_dir / "saves"));
    }
  }
};
