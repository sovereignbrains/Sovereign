#pragma once

#include <optional>
#include <string>

// The config the service starts by itself when Windows boots - before anyone
// signs in and before a browser does (Mullvad's daemon connects the same way):
// the tray's last box_start that asked for it (at_boot), kept until the tray
// says otherwise (box_boot). Before 0.5.8 the service kept nothing on disk
// (issue #8) and the tunnel came up only once the tray had started after
// sign-in - a browser in the Startup folder was faster (16.10.2026).
//
// The config holds the servers' keys: the file is encrypted with DPAPI's
// machine key and readable by SYSTEM and Administrators only - the folder
// itself (%ProgramData%\Sovereign) lets every user read.

namespace sovereign::service {

class IBootStore {
 public:
  IBootStore() = default;
  virtual ~IBootStore() = default;
  IBootStore(const IBootStore&) = delete;
  IBootStore& operator=(const IBootStore&) = delete;
  IBootStore(IBootStore&&) = delete;
  IBootStore& operator=(IBootStore&&) = delete;

  // Keeps `config` for the next boot, replacing the one there: "" or why not.
  virtual std::string Save(const std::string& config) = 0;
  // No config at boot any more. Nothing kept is fine.
  virtual void Forget() = 0;
  // The kept config; nullopt - none, or unreadable (then it's as if none).
  virtual std::optional<std::string> Load() = 0;
};

// Who may touch the file: SYSTEM and Administrators, nothing inherited.
inline constexpr wchar_t kBootStoreSddl[] = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";

// %ProgramData%\Sovereign\boot-config.bin; empty if %ProgramData% is unknown.
std::wstring BootStorePath();

class FileBootStore final : public IBootStore {
 public:
  // `sddl`: the file's security - kBootStoreSddl in the service, the owner's
  // own in tests (they don't run as SYSTEM).
  FileBootStore(std::wstring path, std::wstring sddl);

  std::string Save(const std::string& config) override;
  void Forget() override;
  std::optional<std::string> Load() override;

 private:
  std::wstring path_;
  std::wstring sddl_;
};

}  // namespace sovereign::service
