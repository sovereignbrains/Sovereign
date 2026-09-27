#pragma once

// windows.h's min/max macros break std::min/max (see flyout.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui_content.h"

namespace sovereign::tray {

// The main window: everything the flyout does, with room - a navigation rail
// on the left (overview, protocol, subscription, apps, log, settings) and the
// page on the right. Drawn with Direct2D/DirectWrite in DIPs like the flyout,
// in the same palette (ui_style.h); the log is a read-only edit box, so its
// text selects and copies like any other.
//
// Closing the window hides it: the tray keeps running, and the flyout's
// "open" or a second start of sovereign-tray.exe brings it back. Tab and
// Shift+Tab move the focus, Enter or Space activate, Ctrl+1..6 switch pages.
// UI thread only.
class MainWindow {
 public:
  using CommandHandler = std::function<void(UiCommand command, const UiArgs& args)>;

  MainWindow(HINSTANCE instance, CommandHandler onCommand);
  ~MainWindow();
  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;
  MainWindow(MainWindow&&) = delete;
  MainWindow& operator=(MainWindow&&) = delete;

  // Shows the window on `page` and brings it to the front.
  void Show(UiPage page);
  void Hide();
  bool IsVisible() const;
  // New content; repaints if visible.
  void Update(const UiContent& content);

  // The log page's lines: all of them, or new ones at the bottom.
  void SetLogs(const std::vector<std::wstring>& lines);
  void AppendLogs(const std::vector<std::wstring>& lines);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Draws `page` with `content` into a PNG of `width` x `height` pixels at
// `dpi`, with no window - for tests/ui's snapshots, which let a change to the
// window be looked at without a desktop. `logs` stand in for the edit box.
// Throws on failure.
void RenderMainWindowSnapshot(const UiContent& content, UiPage page, const std::vector<std::wstring>& logs,
                              UINT width, UINT height, float dpi, const std::wstring& pngPath);

}  // namespace sovereign::tray
