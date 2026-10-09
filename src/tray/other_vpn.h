#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Another VPN in the way: when the connection can't come up with the kill
// switch holding the internet shut (main.cpp UpdateLockout), the tray looks
// once for one - a VPN program running, or a VPN's adapter up - and names it,
// so the user turns it off instead of blaming the provider. Never fought:
// its filters are its own (a kill switch removed behind its back would be
// put back, and a work VPN is wanted). Checked only then, nothing in the
// background.
//
// The pure part is unit-tested in tests/unit/other_vpn_test.cpp; what Windows
// has (NetAdapters, RunningProcesses) is other_vpn_win.cpp's.

namespace sovereign::tray {

struct NetAdapter {
  std::string name;         // "AmneziaVPN" - the friendly name
  std::string description;  // "WireGuard Tunnel"
  std::uint32_t ifType = 0;  // IANA ifType: 53 virtual, 131 tunnel, 23 PPP
  bool up = false;
  std::vector<std::string> ipv4;
};

// The VPN in the way, as the user knows it ("AmneziaVPN"). `sure`: its
// adapter is up - not ours (an address in `ownTun`, our TUN's 172.19.0.0/30),
// not Windows' own tunnels (Teredo, ISATAP, IP-HTTPS). Else a VPN program
// among `processes` (exe names, any case) - only maybe: open isn't connected,
// though a kill switch can hold with its tunnel down. Its service alone
// (AmneziaVPN-service) runs always and isn't counted.
struct OtherVpnFound {
  std::string name;
  bool sure = false;
  friend bool operator==(const OtherVpnFound&, const OtherVpnFound&) = default;
};
std::optional<OtherVpnFound> OtherVpn(const std::vector<NetAdapter>& adapters, const std::vector<std::string>& processes,
                                      std::string_view ownTun = "172.19.0.");

// What Windows has now (other_vpn_win.cpp); empty if it can't be read.
std::vector<NetAdapter> NetAdapters();
std::vector<std::string> RunningProcesses();

}  // namespace sovereign::tray
