#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Before wil: its wrappers for these come only with them.
#include <userenv.h>
#include <wtsapi32.h>

#include <cstdint>
#include <string>

#include "self_update.h"
#include "update.h"

namespace sovereign::service {

// The real IUpdateRunner: checks the tray's installer against the release's
// checksum on GitHub, copies it into `directory` (UpdateDirectory: under
// Program Files, where users can't write) and runs it silently as SYSTEM.
class SelfUpdate final : public IUpdateRunner {
 public:
  // Clears the installers an update before left in `directory`.
  SelfUpdate(tray::Version current, std::wstring directory, std::wstring userAgent);
  std::string Install(const std::string& version, const std::string& installer, std::uint32_t session) override;

 private:
  const tray::Version current_;
  const std::wstring directory_;
  const std::wstring userAgent_;
};

// <the service's folder>\Update.
std::wstring UpdateDirectory();

// sovereign-core.exe --launch-tray <session>, run by the installer the
// service started: the new tray in the user's session, as the user, without
// its window. False if nobody is signed in there.
bool LaunchTray(std::uint32_t session);

}  // namespace sovereign::service
