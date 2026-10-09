#include "other_vpn.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace sovereign::tray {

namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return out;
}

bool Has(std::string_view text, std::string_view part) { return Lower(text).find(part) != std::string::npos; }

// VPN programs by their exe (lower case), as their users call them.
constexpr std::array<std::pair<std::string_view, std::string_view>, 28> kPrograms = {{
    {"amneziavpn.exe", "AmneziaVPN"},
    {"amneziawg.exe", "AmneziaWG"},
    {"wireguard.exe", "WireGuard"},
    {"openvpn.exe", "OpenVPN"},
    {"openvpn-gui.exe", "OpenVPN"},
    {"openvpnconnect.exe", "OpenVPN Connect"},
    {"mullvad-daemon.exe", "Mullvad VPN"},
    {"nordvpn.exe", "NordVPN"},
    {"protonvpn.exe", "Proton VPN"},
    {"protonvpn.client.exe", "Proton VPN"},
    {"expressvpn.exe", "ExpressVPN"},
    {"surfshark.exe", "Surfshark"},
    {"windscribe.exe", "Windscribe"},
    {"adguardvpn.exe", "AdGuard VPN"},
    {"outline.exe", "Outline"},
    {"hiddify.exe", "Hiddify"},
    {"v2rayn.exe", "v2rayN"},
    {"nekoray.exe", "NekoRay"},
    {"nekobox.exe", "NekoBox"},
    {"throne.exe", "Throne"},
    {"karing.exe", "Karing"},
    {"happ.exe", "Happ"},
    {"flclash.exe", "FlClash"},
    {"clash-verge.exe", "Clash Verge"},
    {"clash verge.exe", "Clash Verge"},
    {"psiphon3.exe", "Psiphon"},
    {"sing-box.exe", "sing-box"},
    {"xray.exe", "Xray"},
}};

bool WindowsOwnTunnel(const NetAdapter& a) {
  return Has(a.description, "teredo") || Has(a.description, "isatap") || Has(a.description, "6to4") ||
         Has(a.description, "ip-https") || Has(a.description, "loopback");
}

bool VpnAdapter(const NetAdapter& a) {
  constexpr std::uint32_t kPpp = 23;
  constexpr std::uint32_t kVirtual = 53;
  constexpr std::uint32_t kTunnel = 131;
  const bool byType = a.ifType == kPpp || a.ifType == kVirtual || a.ifType == kTunnel;
  const bool byName = Has(a.description, "wireguard") || Has(a.description, "wintun") || Has(a.description, "tap-windows") ||
                      Has(a.description, "openvpn") || Has(a.description, "vpn") || Has(a.name, "vpn");
  return byType || byName;
}

}  // namespace

std::optional<OtherVpnFound> OtherVpn(const std::vector<NetAdapter>& adapters, const std::vector<std::string>& processes,
                                      std::string_view ownTun) {
  for (const NetAdapter& a : adapters) {
    const bool ours = std::any_of(a.ipv4.begin(), a.ipv4.end(), [&](const std::string& ip) { return ip.starts_with(ownTun); });
    if (a.up && !ours && !WindowsOwnTunnel(a) && VpnAdapter(a)) {
      // A friendly name the user gave it says more than "Ethernet 3".
      const bool generic = Has(a.name, "ethernet") || Has(a.name, "local area connection") || a.name.empty();
      return OtherVpnFound{.name = generic ? a.description : a.name, .sure = true};
    }
  }
  for (const std::string& process : processes) {
    const std::string exe = Lower(process);
    for (const auto& [name, shown] : kPrograms) {
      if (exe == name) {
        return OtherVpnFound{.name = std::string(shown), .sure = false};
      }
    }
  }
  return std::nullopt;
}

}  // namespace sovereign::tray
