#pragma once

// windows.h's min/max macros break std::min/max: the CI's clang-tidy doesn't see
// the build's global NOMINMAX (see src/common/sha256.h), so every header that
// pulls windows.h in sets it itself.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <memory>

#include "ui_content.h"

namespace sovereign::tray {

// The Windows 11-style panel the tray icon opens: a borderless dark popup
// with rounded corners (DWM), drawn with Direct2D/DirectWrite in DIPs so it is
// sharp at any DPI, glyphs from Segoe Fluent Icons. Pages: the main one, the
// per-app list, the protocol pick - switched inside, the window resizes to the
// page. Opens above the tray icon, closes when it loses activation or on Esc.
// Lives on the UI thread.
class Flyout {
 public:
  using CommandHandler = std::function<void(UiCommand command, const UiArgs& args)>;

  Flyout(HINSTANCE instance, CommandHandler onCommand);
  ~Flyout();
  Flyout(const Flyout&) = delete;
  Flyout& operator=(const Flyout&) = delete;
  Flyout(Flyout&&) = delete;
  Flyout& operator=(Flyout&&) = delete;

  // Opens (on the main page) next to `anchor`, the tray icon's screen rect,
  // or closes if open.
  void Toggle(const RECT& anchor, const UiContent& content);
  void Hide();
  // New content; relayouts and repaints if open.
  void Update(const UiContent& content);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sovereign::tray
