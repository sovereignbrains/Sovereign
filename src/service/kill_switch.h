#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The kill switch: while it's on, the machine's traffic goes through the
// tunnel or nowhere. It's the Windows Filtering Platform's to enforce
// (kill_switch_wfp.h) - persistent filters, so a crashed core, a crashed
// service or a reboot mid-session doesn't let traffic out around the proxy
// either; they're lifted when the user turns the connection off, turns the
// switch off, or uninstalls (and by sovereign-core.exe --unblock).
//
// What gets through, and nothing else (outbound connections, IPv4 and IPv6):
//   - sovereign-core.exe itself - the core dials the proxies directly, and
//     the apps routed "direct" go out through it too;
//   - loopback, and DHCP;
//   - anything whose local address is the tunnel's (the TUN inbound's
//     addresses, from the running config);
//   - with allowLan, private and link-local destinations (printers, NAS) -
//     but not DNS there: the router answering names would be a leak.
//
// The local network closed (lanClosed) is the same filters' other job, with
// the kill switch or without: nothing of the local network gets to the PC
// and the PC gets to nothing of it - the core included, inbound too - but
// for the addresses the user let in (lanAllowed), and what the network can't
// work without: DHCP, IPv6's neighbour discovery. The tunnel's own addresses
// sit in a private range (172.19.0.1/30) and stay open. DNS to the local
// network stays shut even to an address let in: the router answering names
// would be a leak.
//
// This header is the part that isn't Windows': the rules as data, built from
// the settings and the config - unit-tested in tests/unit/kill_switch_test.cpp.

namespace sovereign::service {

struct KillSwitchSettings {
  bool enabled = false;
  bool allowLan = true;
  bool lanClosed = false;
  std::vector<std::string> lanAllowed;  // addresses or subnets ("192.168.31.1"), each ParsePrefix's
  friend bool operator==(const KillSwitchSettings&, const KillSwitchSettings&) = default;
};

// At most this many addresses are let into a closed local network (a filter
// holds that many alternatives).
inline constexpr std::size_t kMaxLanAllowed = 12;

// An address prefix: 4 or 16 bytes, network byte order.
struct Prefix {
  bool v6 = false;
  std::array<std::uint8_t, 16> bytes{};
  int bits = 0;
  friend bool operator==(const Prefix&, const Prefix&) = default;
};

// "172.19.0.1/30", "fdfe:dcba:9876::1/126"; a bare address is a /32 (/128).
std::optional<Prefix> ParsePrefix(std::string_view text);

// The addresses of the config's TUN inbounds: "address" (sing-box 1.10+),
// and the older "inet4_address"/"inet6_address"; strings or lists of them.
// What doesn't parse is skipped.
std::vector<Prefix> TunPrefixes(std::string_view config);

// One filter: what it matches (every condition kind present must match; the
// prefixes of one kind are alternatives) and what it does.
struct KillSwitchRule {
  bool v6 = false;
  bool permit = false;
  std::uint8_t weight = 0;     // higher wins within the kill switch
  bool inbound = false;        // a connection to the PC (else one it makes)
  bool coreApp = false;        // the connection is sovereign-core.exe's
  bool loopback = false;
  std::vector<Prefix> localPrefixes;
  std::vector<Prefix> remotePrefixes;
  std::optional<std::uint16_t> remotePort;
  std::vector<std::uint16_t> localPorts;  // alternatives; for ICMP, its types
  std::optional<std::uint8_t> protocol;  // IPPROTO_*: 6 TCP, 17 UDP, 58 ICMPv6
  std::string name;            // for the filter's display name
};

// The filters for `settings` over a tunnel with `tun`'s addresses; none when
// the switch is off and the local network open.
std::vector<KillSwitchRule> BuildKillSwitchRules(const KillSwitchSettings& settings, const std::vector<Prefix>& tun);

// What enforces the rules - WfpKillSwitch in the service, a fake in tests.
class IKillSwitch {
 public:
  IKillSwitch() = default;
  virtual ~IKillSwitch() = default;
  IKillSwitch(const IKillSwitch&) = delete;
  IKillSwitch& operator=(const IKillSwitch&) = delete;
  IKillSwitch(IKillSwitch&&) = delete;
  IKillSwitch& operator=(IKillSwitch&&) = delete;

  // Replaces whatever is in force with `rules` (none: lifts it). Empty on
  // success, otherwise why not - the old rules stay then.
  virtual std::string Apply(const std::vector<KillSwitchRule>& rules) = 0;
  // Whether filters of the kill switch are in place (a service restart
  // finds them there: they're persistent).
  virtual bool Active() = 0;
};

}  // namespace sovereign::service
