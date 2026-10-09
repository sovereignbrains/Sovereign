// Another VPN in the way (src/tray/other_vpn.h): named by its program first,
// else by its adapter; ours and Windows' own tunnels never.

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "check.h"
#include "other_vpn.h"

namespace {

using namespace sovereign::tray;

void TestOtherVpn() {
  const NetAdapter wifi{.name = "Wi-Fi", .description = "Intel(R) Wi-Fi 6E AX210 160MHz", .ifType = 71, .up = true,
                        .ipv4 = {"192.168.31.57"}};
  const NetAdapter ours{.name = "tun0", .description = "sing-tun Tunnel", .ifType = 53, .up = true, .ipv4 = {"172.19.0.1"}};
  const NetAdapter amnezia{.name = "AmneziaVPN", .description = "WireGuard Tunnel", .ifType = 53, .up = true,
                           .ipv4 = {"172.29.172.1"}};
  const NetAdapter teredo{.name = "Teredo", .description = "Teredo Tunneling Pseudo-Interface", .ifType = 131, .up = true,
                          .ipv4 = {}};
  const NetAdapter tap{.name = "Ethernet 3", .description = "TAP-Windows Adapter V9", .ifType = 6, .up = true,
                       .ipv4 = {"10.8.0.2"}};
  const NetAdapter hyperv{.name = "vEthernet (Default Switch)", .description = "Hyper-V Virtual Ethernet Adapter",
                          .ifType = 6, .up = true, .ipv4 = {"172.22.16.1"}};

  // Nothing but ours, Windows' and real cards, its service alone: nobody in the way.
  CHECK(!OtherVpn({wifi, ours, teredo, hyperv}, {"explorer.exe", "sovereign-core.exe", "AmneziaVPN-service.exe"}));
  // Its adapter up: sure, named as the user calls it.
  CHECK((OtherVpn({wifi, ours, amnezia}, {}) == OtherVpnFound{.name = "AmneziaVPN", .sure = true}));
  // A generic name: the description says more.
  CHECK((OtherVpn({wifi, tap}, {}) == OtherVpnFound{.name = "TAP-Windows Adapter V9", .sure = true}));
  // Down, its program open: only maybe (a kill switch can hold with the tunnel down).
  NetAdapter down = amnezia;
  down.up = false;
  CHECK(!OtherVpn({wifi, down}, {}));
  CHECK((OtherVpn({wifi, down}, {"System", "AmneziaVPN.exe"}) == OtherVpnFound{.name = "AmneziaVPN", .sure = false}));
  // An adapter up beats a program open.
  CHECK((OtherVpn({wifi, amnezia}, {"Hiddify.exe"}) == OtherVpnFound{.name = "AmneziaVPN", .sure = true}));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestOtherVpn();
  } catch (const std::exception& e) {
    std::cerr << "exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
