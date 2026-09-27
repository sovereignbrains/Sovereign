#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app_rules.h"

namespace sovereign::tray {

// The tray owns the user's side of things; the service only executes pipe
// commands and keeps nothing on disk (issue #8: the pipe is control only).
// Everything lives in %LOCALAPPDATA%\Sovereign, which only this user (and
// SYSTEM/admins) can read - it holds a config with passwords and keys.
//
//   tray.json    {"wantOn", "subscriptionUrl", "lastRefresh", "updateHours",
//                 "appsMode", "apps", "appPaths", "protocol"}
//   config.json            the sing-box config: the subscription's copy, the
//                          user's edits on it, or by hand when there's no
//                          subscription; the tray adds its own bits (the
//                          protocol pick, the apps, the cache file) when it
//                          sends it to box_start
//   subscription.json      the subscription as it last arrived and was taken:
//                          what config.json is based on (config_sync.h)
//   subscription.new.json  a newer one that waits for the user's choice,
//                          because config.json has edits of their own
//   history\               config.json as it was before each replacement,
//                          the newest kHistoryKeep, named by UTC time

std::filesystem::path DataDir();  // created if missing

struct TraySettings {
  bool wantOn = false;
  std::string subscriptionUrl;       // UTF-8; empty = none, config.json is by hand
  std::int64_t lastRefresh = 0;      // unix seconds of the last successful refresh
  int updateHours = 12;              // from Profile-Update-Interval, else the default
  AppsMode appsMode = AppsMode::Exclude;  // per-app routing (app_rules.h)
  std::vector<std::string> apps;          // exe names, as sing-box's process_name
  // Where a listed app's exe was last seen, by its name in `apps`: only for
  // its icon in the window - the rule matches the name wherever it runs. Not
  // a std::map: MSVC's debug map allocates when moved, and settings move.
  std::vector<std::pair<std::string, std::string>> appPaths;
  std::string protocol;  // the proxy selector's default to use; empty = the config's (protocol_choice.h)
};

// An app's remembered exe path, or null; and setting one (replacing).
const std::string* AppPath(const TraySettings& settings, const std::string& app);
void SetAppPath(TraySettings& settings, const std::string& app, std::string path);

// A missing or unreadable tray.json gives the defaults: a broken settings file
// must not keep the tray from starting.
TraySettings LoadSettings();
// Atomic (write + rename), so a crash mid-save leaves the old file.
void SaveSettings(const TraySettings& settings);

// config.json's text, or nullopt if there is none.
std::optional<std::string> LoadConfig();
// Atomic, like SaveSettings.
void SaveConfig(const std::string& text);

std::optional<std::string> LoadOriginal();
void SaveOriginal(const std::string& text);
std::optional<std::string> LoadPending();
void SavePending(const std::string& text);
void ClearPending();   // no error if there is none
void ClearOriginal();  // likewise: no subscription, no base

// Keeps `text` (config.json about to be replaced) in history\, dropping the
// oldest beyond kHistoryKeep.
inline constexpr std::size_t kHistoryKeep = 20;
void SaveHistory(const std::string& text);

}  // namespace sovereign::tray
