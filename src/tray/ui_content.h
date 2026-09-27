#pragma once

// windows.h's min/max macros break std::min/max (see flyout.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <format>
#include <string>
#include <utility>
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

// What the tray's windows show - the flyout and the main window alike; built
// by main.cpp from the worker's view. Strings are ready to draw.
struct UiContent {
  Display display = Display::ServiceDown;
  std::wstring status;           // "вкл · ↓ 588 Б/с ↑ 13.3 КБ/с · соединений: 21"
  COLORREF statusDot = RGB(150, 150, 150);
  bool on = false;               // the toggle's position (the user's intent)
  double down = 0;               // bytes per second
  double up = 0;
  std::int64_t connections = 0;
  std::wstring error;            // why the box doesn't run; empty if it does or is off
  // The last two minutes of rates, oldest first, one pair a poll: down, up.
  std::vector<std::pair<float, float>> history;

  std::wstring subscription;     // "26.09 19:20" / "нет" / "ошибка: ..."
  bool hasSubscription = false;  // enables the refresh button
  std::wstring subscriptionHost; // the server's name only - the URL's path is a secret
  std::wstring subscriptionError;
  int updateHours = 0;           // the refresh interval; 0 = unknown

  bool appsInclude = false;      // per-app mode: false = all except the list
  std::vector<std::wstring> apps;
  std::vector<std::wstring> protocols;  // the selector's options; empty = no choice
  int protocol = -1;                    // index of the one in use
  std::vector<UiDelay> delays;          // one per protocol (may be shorter: untested)
  bool delaysTesting = false;           // a latency test is running
  bool canTestDelays = false;           // the box runs: its servers can be tested
  std::wstring delayError;              // why the last test didn't start
  bool autostart = false;               // the tray starts when the user signs in
  std::wstring version;                 // "0.1.0 · sing-box 1.14.1"
  UiUpdate update = UiUpdate::Idle;
  std::wstring updateVersion;           // the newest release, once known
  std::wstring updateError;
};

// The main window's pages, in the order of its navigation.
enum class UiPage : std::uint8_t { Overview, Protocol, Subscription, Apps, Logs, Settings };
inline constexpr int kUiPageCount = 6;

enum class UiCommand : std::uint8_t {
  Toggle,
  PasteSubscription,
  RefreshSubscription,
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
