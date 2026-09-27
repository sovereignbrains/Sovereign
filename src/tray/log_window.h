#pragma once

// windows.h's min/max macros break std::min/max (see flyout.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace sovereign::tray {

// The log window: the core's recent lines and the tray's own complaints,
// newest at the bottom, in a read-only text box that selects and copies like
// any other. A plain resizable window with the app icon and a dark title bar;
// closing it only closes the view - the tray keeps collecting lines. Follows
// new lines while scrolled to the bottom, stays put while the user reads
// further up. UI thread only.
class LogWindow {
 public:
  explicit LogWindow(HINSTANCE instance);
  ~LogWindow();
  LogWindow(const LogWindow&) = delete;
  LogWindow& operator=(const LogWindow&) = delete;
  LogWindow(LogWindow&&) = delete;
  LogWindow& operator=(LogWindow&&) = delete;

  // Opens with `lines`, or brings the open window forward with them.
  void Show(const std::vector<std::wstring>& lines);
  bool IsOpen() const;
  void Close();
  // Adds lines at the bottom of an open window.
  void Append(const std::vector<std::wstring>& lines);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sovereign::tray
