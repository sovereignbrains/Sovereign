#pragma once

// windows.h's min/max macros break std::min/max.
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

// The tray's window: compact, one screen for every day - the state and speed
// at the top, tiles into the subscription, the apps, the log and the
// settings, and a bar at the bottom with the switch and the server in use
// (which opens the servers); those pages open in its place, back with the
// arrow or Esc. Drawn with Direct2D/DirectWrite in DIPs, in ui_style.h's
// palette.
//
// A click on the tray icon shows it in the corner by the notification area
// (Toggle); closing it only hides it - the tray keeps running. Tab and
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
  // The tray icon's click: hides the window if it's in front, else shows it
  // (on the overview, unless it was open behind others).
  void Toggle();
  bool IsVisible() const;
  // New content; repaints if visible.
  void Update(const UiContent& content);

  // Typing in place: an edit box over an item of the page shown - the one
  // whose command is `anchor` with `index` (for RenameProfile: the page's
  // title) - holding `initial`. Enter, or clicking elsewhere, sends `onEnter`
  // with the text in UiArgs::text and the same index; Esc drops it.
  void EditInPlace(UiCommand anchor, int index, UiCommand onEnter, const std::wstring& initial, bool digitsOnly);

  // The log page's lines: all of them, or new ones at the bottom.
  void SetLogs(const std::vector<std::wstring>& lines);
  void AppendLogs(const std::vector<std::wstring>& lines);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Draws `page` with `content` into a PNG of `width` x `height` pixels at
// `dpi`, with no window - for tests/ui's snapshots, which let a change to the
// window be looked at without a desktop. `logs` are the log page's lines.
// Throws on failure.
void RenderMainWindowSnapshot(const UiContent& content, UiPage page, const std::vector<std::wstring>& logs,
                              UINT width, UINT height, float dpi, const std::wstring& pngPath);

}  // namespace sovereign::tray
