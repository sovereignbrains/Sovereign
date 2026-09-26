#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sovereign::tray {

// What the flyout shows; built by main.cpp from the worker's View.
struct FlyoutContent {
  std::wstring status;           // "вкл · ↓ 588 Б/с ↑ 13.3 КБ/с · соединений: 21"
  COLORREF statusDot = RGB(150, 150, 150);
  bool on = false;               // the toggle's position (the user's intent)
  std::wstring subscription;     // "26.09 19:20" / "нет" / "ошибка: ..."
  bool hasSubscription = false;  // enables the refresh button
  bool appsInclude = false;      // per-app mode: false = all except the list
  std::vector<std::wstring> apps;
  std::vector<std::wstring> protocols;  // the selector's options; empty = no choice
  int protocol = -1;                    // index of the one in use
};

enum class FlyoutCommand : std::uint8_t {
  Toggle,
  PasteSubscription,
  RefreshSubscription,
  SetAppsMode,   // index: 0 all except the list, 1 only the list
  RemoveApp,     // index into apps
  AddRunning,    // anchor: where to open the list of running programs
  AddExe,
  SetProtocol,   // index into protocols
  OpenFolder,
  Exit,
};

struct FlyoutArgs {
  POINT anchor{};
  int index = 0;
};

// The Windows 11-style panel the tray icon opens: a borderless dark popup
// with rounded corners (DWM), drawn with Direct2D/DirectWrite in DIPs so it is
// sharp at any DPI, glyphs from Segoe Fluent Icons. Pages: the main one, the
// per-app list, the protocol pick - switched inside, the window resizes to the
// page. Opens above the tray icon, closes when it loses activation or on Esc.
// Lives on the UI thread.
class Flyout {
 public:
  using CommandHandler = std::function<void(FlyoutCommand command, const FlyoutArgs& args)>;

  Flyout(HINSTANCE instance, CommandHandler onCommand);
  ~Flyout();
  Flyout(const Flyout&) = delete;
  Flyout& operator=(const Flyout&) = delete;
  Flyout(Flyout&&) = delete;
  Flyout& operator=(Flyout&&) = delete;

  // Opens (on the main page) next to `anchor`, the tray icon's screen rect,
  // or closes if open.
  void Toggle(const RECT& anchor, const FlyoutContent& content);
  void Hide();
  // New content; relayouts and repaints if open.
  void Update(const FlyoutContent& content);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sovereign::tray
