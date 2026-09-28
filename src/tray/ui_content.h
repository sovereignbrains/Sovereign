#pragma once

// windows.h's min/max macros break std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "tray_model.h"

namespace sovereign::tray {

// A server's latency as the windows show it.
struct UiDelay {
  enum class State : std::uint8_t { None, Pending, Ok, Failed };
  State state = State::None;  // None: not tested since the box started
  int ms = 0;
};

// Where the updater is (updater.h), as the windows show it.
enum class UiUpdate : std::uint8_t { Idle, Checking, UpToDate, Available, Downloading, Failed };

// What the tray's window shows; built by main.cpp from the worker's view.
// Strings are ready to draw.
struct UiContent {
  Display display = Display::ServiceDown;
  bool on = false;               // the toggle's position (the user's intent)
  double down = 0;               // bytes per second
  double up = 0;
  std::int64_t connections = 0;
  std::wstring error;            // why the box doesn't run; empty if it does or is off

  std::wstring subscription;     // "26.09 19:20" / "нет" / "ошибка: ..."
  bool hasSubscription = false;  // enables the refresh button
  bool hasConfig = false;        // config.json is there (a subscription's or the user's own)
  std::wstring subscriptionHost; // the server's name only - the URL's path is a secret
  std::wstring subscriptionError;
  int updateHours = 0;           // the refresh interval; 0 = unknown
  bool configEdited = false;     // config.json has edits of the user's own (config_sync.h)
  bool subscriptionWaiting = false;  // a newer subscription waits for the user's choice
  std::wstring choiceError;          // why that choice didn't go through
  std::vector<std::wstring> mergeNotes;  // where carrying the edits over met the subscription's changes

  bool appsInclude = false;      // per-app mode: false = all except the list
  std::vector<std::wstring> apps;
  std::vector<std::wstring> appPaths;   // per app: its exe's full path for the icon; empty if unknown
  std::vector<std::wstring> protocols;  // the selector's options; empty = no choice
  int protocol = -1;                    // index of the one in use
  std::vector<UiDelay> delays;          // one per protocol (may be shorter: untested)
  bool delaysTesting = false;           // a latency test is running
  bool canTestDelays = false;           // the box runs: its servers can be tested
  std::wstring delayError;              // why the last test didn't start
  // The exit through the server in use (while on): address, country.
  std::wstring exitIp;                  // empty until known
  std::wstring exitCountry;             // "NL", or empty
  std::wstring exitCountryName;         // "Нидерланды" - the system's name for it
  bool exitPending = false;             // being looked up
  bool hideExitIp = false;              // the country only
  std::wstring logLevel;                // what the core writes; empty = the config's
  bool killSwitch = false;              // the setting
  bool killSwitchLan = true;
  bool killSwitchActive = false;        // the filters are in place
  std::wstring killSwitchError;         // why the service couldn't apply it
  bool autostart = false;               // the tray starts when the user signs in
  std::wstring version;                 // "0.1.0 · sing-box 1.14.1"
  UiUpdate update = UiUpdate::Idle;
  std::wstring updateVersion;           // the newest release, once known
  std::wstring updateError;
};

// The window's pages: the overview, and the ones it opens (back with Esc).
enum class UiPage : std::uint8_t { Overview, Servers, Subscription, Apps, Logs, Settings };
inline constexpr int kUiPageCount = 6;

enum class UiCommand : std::uint8_t {
  Toggle,
  PasteSubscription,
  RefreshSubscription,
  TakeSubscription,  // the waiting subscription replaces the edited config
  KeepConfig,        // the edited config stays
  CarryOverEdits,    // the edits, merged into the waiting subscription
  RevertConfig,      // the config back to the subscription as it arrived
  ImportFile,        // a config of the user's own from a .json file
  CopySubscription,  // the subscription link onto the clipboard
  RemoveSubscription,  // no more refreshes; the config stays
  ToggleExitIp,        // show or hide the exit's address
  ChooseLogLevel,      // anchor: where to open the menu of levels
  ToggleKillSwitch,
  ToggleKillSwitchLan,
  SetAppsMode,   // index: 0 all except the list, 1 only the list
  RemoveApp,     // index into apps
  AddRunning,    // anchor: where to open the list of running programs
  AddExe,
  SetProtocol,   // index into protocols
  ToggleAutostart,
  TestDelays,
  CheckUpdate,
  InstallUpdate,
  OpenWindow,    // index: the UiPage to show
  OpenFolder,
  Exit,
};

struct UiArgs {
  POINT anchor{};
  int index = 0;
  HWND owner = nullptr;  // the window a menu or a dialog belongs to
};

// "48 мс", "нет ответа", "…" or nothing.
inline std::wstring DelayLabel(const UiDelay& delay) {
  switch (delay.state) {
    case UiDelay::State::None: return {};
    case UiDelay::State::Pending: return L"…";
    case UiDelay::State::Ok: return std::format(L"{} мс", delay.ms);
    case UiDelay::State::Failed: return L"нет ответа";
  }
  return {};
}

inline std::wstring FormatRate(double bytesPerSecond) {
  if (bytesPerSecond < 1024) {
    return std::format(L"{:.0f} Б/с", bytesPerSecond);
  }
  if (bytesPerSecond < 1024 * 1024) {
    return std::format(L"{:.1f} КБ/с", bytesPerSecond / 1024);
  }
  return std::format(L"{:.1f} МБ/с", bytesPerSecond / (1024 * 1024));
}

}  // namespace sovereign::tray
