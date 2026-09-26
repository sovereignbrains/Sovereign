#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sovereign::tray {

// What the flyout shows; built by main.cpp from the worker's View.
struct FlyoutContent {
  std::wstring status;           // "вкл · ↓ 588 Б/с ↑ 13.3 КБ/с · соединений: 21"
  COLORREF statusDot = RGB(150, 150, 150);
  bool on = false;               // the toggle's position (the user's intent)
  std::wstring subscription;     // "обновлена 26.09 19:20" / "нет" / "ошибка: ..."
  bool hasSubscription = false;  // enables the refresh button
  std::wstring apps;             // "Приложения (3) · кроме списка"
};

enum class FlyoutCommand : std::uint8_t {
  Toggle,
  PasteSubscription,
  RefreshSubscription,
  Apps,  // anchor: where to open the apps menu
  OpenFolder,
  Exit,
};

// The Windows 11-style panel the tray icon opens: a borderless dark popup
// with rounded corners (DWM), drawn with Direct2D/DirectWrite in DIPs so it
// is sharp at any DPI, glyphs from Segoe Fluent Icons. Opens above the tray
// icon, closes when it loses activation or on Esc. Lives on the UI thread.
class Flyout {
 public:
  using CommandHandler = std::function<void(FlyoutCommand command, POINT anchor)>;

  Flyout(HINSTANCE instance, CommandHandler onCommand);
  ~Flyout();
  Flyout(const Flyout&) = delete;
  Flyout& operator=(const Flyout&) = delete;
  Flyout(Flyout&&) = delete;
  Flyout& operator=(Flyout&&) = delete;

  // Opens next to `anchor` (the tray icon's screen rect), or closes if open.
  void Toggle(const RECT& anchor, const FlyoutContent& content);
  void Hide();
  // New content; repaints if open.
  void Update(const FlyoutContent& content);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sovereign::tray
