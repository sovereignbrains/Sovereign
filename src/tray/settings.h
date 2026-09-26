#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace sovereign::tray {

// The tray owns the user's side of things; the service only executes pipe
// commands and keeps nothing on disk (issue #8: the pipe is control only).
// Everything lives in %LOCALAPPDATA%\Sovereign, which only this user (and
// SYSTEM/admins) can read - it holds a config with passwords and keys.
//
//   tray.json    {"wantOn", "subscriptionUrl", "lastRefresh", "updateHours"}
//   config.json  the sing-box config box_start sends: written by the
//                subscription refresh, or by hand when there's no subscription

std::filesystem::path DataDir();  // created if missing

struct TraySettings {
  bool wantOn = false;
  std::string subscriptionUrl;       // UTF-8; empty = none, config.json is by hand
  std::int64_t lastRefresh = 0;      // unix seconds of the last successful refresh
  int updateHours = 12;              // from Profile-Update-Interval, else the default
};

// A missing or unreadable tray.json gives the defaults: a broken settings file
// must not keep the tray from starting.
TraySettings LoadSettings();
// Atomic (write + rename), so a crash mid-save leaves the old file.
void SaveSettings(const TraySettings& settings);

// config.json's text, or nullopt if there is none.
std::optional<std::string> LoadConfig();
// Atomic, like SaveSettings.
void SaveConfig(const std::string& text);

}  // namespace sovereign::tray
