#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app_rules.h"
#include "profiles.h"

namespace sovereign::tray {

// The tray owns the user's side of things; the service only executes pipe
// commands and keeps nothing on disk (issue #8: the pipe is control only).
// Everything lives in %LOCALAPPDATA%\Sovereign, which only this user (and
// SYSTEM/admins) can read - it holds configs with passwords and keys.
//
//   tray.json    {"wantOn", "profiles", "protocol", "appsMode", "apps",
//                 "appPaths", "hideExitIp", "logLevel", "killSwitch",
//                 "killSwitchLan"}
//   profiles\<id>\         one configuration (profiles.h), each with:
//     config.json            the sing-box config: the subscription's copy,
//                            the user's edits on it, or the user's own; the
//                            tray adds its own bits (the protocol pick, the
//                            apps, the cache file) when it sends it to box_start
//     subscription.json      the subscription as it last arrived and was
//                            taken: what config.json is based on (config_sync.h)
//     subscription.new.json  a newer one that waits for the user's choice,
//                            because config.json has edits of their own
//     history\               config.json as it was before each replacement,
//                            the newest kHistoryKeep, named by UTC time
//   history\     the config.json of each configuration removed
//
// A tray from before profiles kept one configuration in the folder itself
// (config.json... and "subscriptionUrl" in tray.json): LoadSettings moves it
// into profiles\p1\ once.

std::filesystem::path DataDir();  // created if missing
// profiles\<id>\, created if missing; `id` must be IsProfileId.
std::filesystem::path ProfileDir(const std::string& id);
// The folder and all in it; no error if it isn't there.
void RemoveProfileDir(const std::string& id);

struct TraySettings {
  bool wantOn = false;
  std::vector<Profile> profiles;  // in the order the window lists them
  // The proxy selector's option to use in the config the box runs (all the
  // configurations on, combine.h); empty = the config's default (protocol_choice.h).
  std::string protocol;
  AppsMode appsMode = AppsMode::Exclude;  // per-app routing (app_rules.h)
  std::vector<std::string> apps;          // exe names, as sing-box's process_name
  // Where a listed app's exe was last seen, by its name in `apps`: only for
  // its icon in the window - the rule matches the name wherever it runs. Not
  // a std::map: MSVC's debug map allocates when moved, and settings move.
  std::vector<std::pair<std::string, std::string>> appPaths;
  bool hideExitIp = false;  // the window shows the exit's country, not its address
  std::string logLevel;     // what the core writes (log_level.h); empty = the config's
  // The kill switch (the service's kill_switch.h): in force while the
  // connection is meant to be on, so a drop doesn't let traffic around it.
  bool killSwitch = false;
  bool killSwitchLan = true;  // the local network stays reachable
};

// An app's remembered exe path, or null; and setting one (replacing).
const std::string* AppPath(const TraySettings& settings, const std::string& app);
void SetAppPath(TraySettings& settings, const std::string& app, std::string path);

// A missing or unreadable tray.json gives the defaults: a broken settings file
// must not keep the tray from starting. Moves a tray's single configuration
// from before profiles into one (see above).
TraySettings LoadSettings();
// Atomic (write + rename), so a crash mid-save leaves the old file.
void SaveSettings(const TraySettings& settings);

// A profile's files, in `dir` (ProfileDir). config.json's text, or nullopt
// if there is none.
std::optional<std::string> LoadConfig(const std::filesystem::path& dir);
// Atomic, like SaveSettings.
void SaveConfig(const std::filesystem::path& dir, const std::string& text);

std::optional<std::string> LoadOriginal(const std::filesystem::path& dir);
void SaveOriginal(const std::filesystem::path& dir, const std::string& text);
std::optional<std::string> LoadPending(const std::filesystem::path& dir);
void SavePending(const std::filesystem::path& dir, const std::string& text);
void ClearPending(const std::filesystem::path& dir);   // no error if there is none
void ClearOriginal(const std::filesystem::path& dir);  // likewise: no subscription, no base

// Keeps `text` (a config.json about to be replaced, or removed) in
// `dir`\history\, dropping the oldest beyond kHistoryKeep.
inline constexpr std::size_t kHistoryKeep = 20;
void SaveHistory(const std::filesystem::path& dir, const std::string& text);

}  // namespace sovereign::tray
