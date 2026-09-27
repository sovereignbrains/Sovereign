#pragma once

// windows.h's min/max macros break std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "update.h"

namespace sovereign::tray {

// Keeps Sovereign up to date from this repository's GitHub releases: checks a
// minute after start and then every 12 hours (or when asked), and on request
// downloads the new installer, checks it against the release's .sha256 and
// leaves it ready to run. Running it is the UI thread's job (LaunchInstaller):
// it needs the user's consent through UAC. All network work happens on the
// updater's own thread, never the tray's worker - a download takes a while.
class Updater {
 public:
  enum class Status : std::uint8_t { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };
  struct State {
    Status status = Status::Idle;
    std::string latest;      // the newest release's version, once known
    std::string error;       // Failed, or Available after a failed download: why
    std::wstring installer;  // Ready: the verified installer on disk
  };

  // `changed` runs on the updater's thread after every state change.
  Updater(Version current, std::wstring userAgent, std::function<void()> changed);
  ~Updater();
  Updater(const Updater&) = delete;
  Updater& operator=(const Updater&) = delete;
  Updater(Updater&&) = delete;
  Updater& operator=(Updater&&) = delete;

  void Check();
  // Downloads the available release; nothing if none is known.
  void Install();
  // The user declined UAC or the installer didn't start: back to Available.
  void LaunchFailed(const std::string& why);
  State Get() const;

 private:
  void Run(const std::stop_token& stop);
  void DoCheck();
  void DoInstall();
  void Set(const std::function<void(State&)>& change);

  const Version current_;
  const std::wstring userAgent_;
  const std::function<void()> changed_;
  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  bool checkRequested_ = false;
  bool installRequested_ = false;
  State state_;
  std::optional<Release> release_;  // the newer release, once found
  std::jthread thread_;             // last: starts once everything above exists
};

// Runs a downloaded installer elevated (UAC) and without its wizard: it
// stops the tray and the service, replaces the files and starts both again.
// Empty on success, otherwise why not ("отменено" when the user said no).
std::string LaunchInstaller(HWND owner, const std::wstring& path);

}  // namespace sovereign::tray
