#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace sovereign::tray {

// The tray owns the user's side of things; the service only executes pipe
// commands and keeps nothing on disk (issue #8: the pipe is control only).
// Everything lives in %LOCALAPPDATA%\Sovereign, which only this user (and
// SYSTEM/admins) can read - it holds a config with passwords and keys.
//
//   tray.json    {"wantOn": bool}
//   config.json  the sing-box config box_start sends (written by the
//                subscription import, T2; for now put there by hand)

std::filesystem::path DataDir();  // created if missing

struct TraySettings {
  bool wantOn = false;
};

// A missing or unreadable tray.json gives the defaults: a broken settings file
// must not keep the tray from starting.
TraySettings LoadSettings();
// Atomic (write + rename), so a crash mid-save leaves the old file.
void SaveSettings(const TraySettings& settings);

// config.json's text, or nullopt if there is none.
std::optional<std::string> LoadConfig();

}  // namespace sovereign::tray
