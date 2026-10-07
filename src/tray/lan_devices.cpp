// LanDevices (lan.h): the PC's IPv4 neighbours on its hardware adapters, and
// its router, from the IP helper's tables.

#include "lan.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <string>

namespace sovereign::tray {

namespace {

std::string Address(const IN_ADDR& address) {
  const auto* b = reinterpret_cast<const unsigned char*>(&address.S_un.S_addr);
  return std::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
}

// A real network card (Wi-Fi, Ethernet), not a tunnel's or a virtual one.
bool Hardware(NET_IFINDEX index, std::map<NET_IFINDEX, bool>& known) {
  if (const auto it = known.find(index); it != known.end()) {
    return it->second;
  }
  MIB_IF_ROW2 row{};
  row.InterfaceIndex = index;
  const bool hardware = GetIfEntry2(&row) == NO_ERROR && row.InterfaceAndOperStatusFlags.HardwareInterface != FALSE;
  known.emplace(index, hardware);
  return hardware;
}

}  // namespace

std::vector<LanDevice> LanDevices() {
  std::vector<LanDevice> devices;
  std::map<NET_IFINDEX, bool> hardware;

  // The router: the next hop of a default route on a hardware adapter.
  std::string router;
  if (PMIB_IPFORWARD_TABLE2 routes = nullptr; GetIpForwardTable2(AF_INET, &routes) == NO_ERROR) {
    ULONG best = ~0UL;
    for (ULONG i = 0; i < routes->NumEntries; ++i) {
      const MIB_IPFORWARD_ROW2& r = routes->Table[i];
      if (r.DestinationPrefix.PrefixLength == 0 && r.NextHop.Ipv4.sin_addr.S_un.S_addr != 0 &&
          Hardware(r.InterfaceIndex, hardware) && r.Metric < best) {
        best = r.Metric;
        router = Address(r.NextHop.Ipv4.sin_addr);
      }
    }
    FreeMibTable(routes);
  }

  if (PMIB_IPNET_TABLE2 table = nullptr; GetIpNetTable2(AF_INET, &table) == NO_ERROR) {
    for (ULONG i = 0; i < table->NumEntries; ++i) {
      const MIB_IPNET_ROW2& n = table->Table[i];
      // A unicast device that answered, on a network card: not broadcast or
      // multicast (their MACs have the group bit), not one never resolved.
      if (n.State == NlnsUnreachable || n.State == NlnsIncomplete || n.PhysicalAddressLength != 6 ||
          (n.PhysicalAddress[0] & 1) != 0 || !Hardware(n.InterfaceIndex, hardware)) {
        continue;
      }
      const std::string address = Address(n.Address.Ipv4.sin_addr);
      if (!LanAddress(address)) {
        continue;
      }
      const unsigned char* m = n.PhysicalAddress;
      devices.push_back({.address = address,
                         .mac = std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", m[0], m[1], m[2], m[3], m[4], m[5]),
                         .router = address == router});
    }
    FreeMibTable(table);
  }
  if (!router.empty() && LanAddress(router) &&
      std::none_of(devices.begin(), devices.end(), [](const LanDevice& d) { return d.router; })) {
    devices.push_back({.address = router, .mac = {}, .router = true});
  }
  return devices;
}

}  // namespace sovereign::tray
