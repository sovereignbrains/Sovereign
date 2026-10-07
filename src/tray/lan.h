#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The local network, closed (settings.h lanClosed): the addresses the user
// lets in, and the devices around to pick them from. The service's filters
// do the closing (src/service/kill_switch.h); this is what the window shows.
//
// The pure part is unit-tested in tests/unit/lan_test.cpp; LanDevices is
// Windows' (lan_devices.cpp).

namespace sovereign::tray {

// An address let into a closed local network, and what the window called it
// then ("Роутер"; empty: just the address) - it may not be around later to
// be named again.
struct LanHost {
  std::string address;  // "192.168.31.1", "192.168.31.0/24"
  std::string name;
  friend bool operator==(const LanHost&, const LanHost&) = default;
};

// At most this many are let in: the service's limit (kill_switch.h).
inline constexpr std::size_t kMaxLanAllowed = 12;

// A device the PC has seen on its own network lately.
struct LanDevice {
  std::string address;  // "192.168.31.45"
  std::string mac;      // "a4:5e:60:12:34:56"; empty if not known
  bool router = false;  // the PC's default gateway
};

// What the user typed, as an address to let in: an IPv4 address or subnet
// ("/24") in the local network's ranges (10/8, 172.16/12, 192.168/16,
// 169.254/16), written the usual way - or nullopt when it isn't one.
std::optional<std::string> LanAddress(std::string_view text);

// One line of the window's list: a device around, or an address let in that
// isn't around now.
struct LanEntry {
  std::string address;
  std::string name;  // the router's, or the one it was let in under; may be empty
  std::string mac;
  bool router = false;
  bool allowed = false;
  bool seen = false;  // among the devices around now
};

// The router first, then what's let in, then the rest - by address.
std::vector<LanEntry> LanEntries(const std::vector<LanDevice>& seen, const std::vector<LanHost>& allowed);

// The devices the PC knows of on its own networks (its IPv4 neighbour
// table, hardware adapters only - not the tunnel), the router among them
// even when it isn't in the table yet.
std::vector<LanDevice> LanDevices();

}  // namespace sovereign::tray
