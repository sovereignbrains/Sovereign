#pragma once

#include <string>
#include <vector>

#include "kill_switch.h"

// The kill switch over the Windows Filtering Platform (kill_switch.h says
// what it lets through): persistent filters at ALE_AUTH_CONNECT (IPv4, IPv6)
// in a sublayer of Sovereign's own, under a provider of its own - so they
// outlive the service process and a reboot, and nothing else's permits
// outweigh them. The filters have fixed keys, so a replacement is a delete
// and an add in one transaction: never a moment with none, never a mix.
// Needs admin rights: the service runs as SYSTEM.

namespace sovereign::service {

class WfpKillSwitch final : public IKillSwitch {
 public:
  // `corePath`: sovereign-core.exe's full path - its connections pass.
  explicit WfpKillSwitch(std::wstring corePath);

  std::string Apply(const std::vector<KillSwitchRule>& rules) override;
  bool Active() override;

 private:
  std::wstring corePath_;
};

// Lifts the kill switch, whatever state the service is in (--uninstall,
// --unblock). Empty on success, including when there was nothing to lift.
std::string RemoveKillSwitch();

}  // namespace sovereign::service
