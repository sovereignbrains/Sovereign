// The kill switch's rules (src/service/kill_switch.h): address prefixes, the
// tunnel's addresses from a config, and the filters for the settings.

#include <algorithm>
#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "kill_switch.h"

namespace {

using sovereign::service::BuildKillSwitchRules;
using sovereign::service::KillSwitchRule;
using sovereign::service::KillSwitchSettings;
using sovereign::service::ParsePrefix;
using sovereign::service::Prefix;
using sovereign::service::TunPrefixes;

void TestPrefixes() {
  const auto v4 = ParsePrefix("172.19.0.1/30");
  CHECK(v4 && !v4->v6 && v4->bits == 30 && v4->bytes[0] == 172 && v4->bytes[1] == 19 && v4->bytes[3] == 1);
  const auto bare = ParsePrefix("8.8.8.8");
  CHECK(bare && bare->bits == 32);
  const auto v6 = ParsePrefix("fdfe:dcba:9876::1/126");
  CHECK(v6 && v6->v6 && v6->bits == 126 && v6->bytes[0] == 0xFD && v6->bytes[1] == 0xFE && v6->bytes[5] == 0x76 &&
        v6->bytes[15] == 1 && v6->bytes[14] == 0);
  const auto full = ParsePrefix("2001:db8:0:0:0:0:0:1");
  CHECK(full && full->bits == 128 && full->bytes[3] == 0xB8 && full->bytes[15] == 1);
  const auto any = ParsePrefix("::/0");
  CHECK(any && any->v6 && any->bits == 0);
  for (const char* bad : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "01.2.3.4", "1.2.3.4/33", "1.2.3.4/", "::1::2",
                          "fe80::/129", "12345::", "1:2:3:4:5:6:7:8:9", "a:", "x.y.z.w", "1.2.3.4/-1"}) {
    CHECK(!ParsePrefix(bad));
  }
}

void TestTunPrefixes() {
  const auto modern = TunPrefixes(
      R"({"inbounds":[{"type":"mixed","listen":"127.0.0.1"},)"
      R"({"type":"tun","address":["172.19.0.1/30","fdfe:dcba:9876::1/126"],"auto_route":true}]})");
  CHECK(modern.size() == 2 && !modern[0].v6 && modern[1].v6);
  const auto legacy = TunPrefixes(R"({"inbounds":[{"type":"tun","inet4_address":"172.19.0.1/30",)"
                                  R"("inet6_address":["fdfe::1/126","junk"]}]})");
  CHECK(legacy.size() == 2);
  CHECK(TunPrefixes(R"({"inbounds":[{"type":"mixed"}]})").empty());
  CHECK(TunPrefixes("not json").empty());
  CHECK(TunPrefixes(R"({"inbounds":"x"})").empty());
}

std::size_t Count(const std::vector<KillSwitchRule>& rules, bool v6) {
  return static_cast<std::size_t>(std::count_if(rules.begin(), rules.end(), [&](const auto& r) { return r.v6 == v6; }));
}

void TestRules() {
  CHECK(BuildKillSwitchRules({.enabled = false, .allowLan = true, .lanClosed = false, .lanAllowed = {}}, {}).empty());

  const auto tun = TunPrefixes(R"({"inbounds":[{"type":"tun","address":["172.19.0.1/30"]}]})");
  const auto rules = BuildKillSwitchRules({.enabled = true, .allowLan = true, .lanClosed = false, .lanAllowed = {}}, tun);
  // IPv4: core, loopback, tunnel, DHCP, LAN DNS block, LAN, block all; IPv6 has no tunnel address here.
  CHECK(Count(rules, false) == 7);
  CHECK(Count(rules, true) == 6);
  for (const bool v6 : {false, true}) {
    // The block-all is the lowest, the core's permit the highest.
    std::uint8_t lowest = 255;
    std::uint8_t highest = 0;
    const KillSwitchRule* blockAll = nullptr;
    for (const auto& r : rules) {
      if (r.v6 != v6) {
        continue;
      }
      lowest = std::min(lowest, r.weight);
      highest = std::max(highest, r.weight);
      if (!r.permit && r.remotePrefixes.empty() && !r.remotePort) {
        blockAll = &r;
      }
    }
    CHECK(blockAll != nullptr && blockAll->weight == lowest);
    const auto core = std::find_if(rules.begin(), rules.end(), [&](const auto& r) { return r.v6 == v6 && r.coreApp; });
    CHECK(core != rules.end() && core->permit && core->weight == highest);
  }
  // DNS in the local network is blocked above the local network's permit.
  const auto dns = std::find_if(rules.begin(), rules.end(), [](const auto& r) { return !r.v6 && r.remotePort == 53; });
  const auto lan = std::find_if(rules.begin(), rules.end(),
                                [](const auto& r) { return !r.v6 && r.permit && !r.remotePrefixes.empty(); });
  CHECK(dns != rules.end() && !dns->permit && lan != rules.end() && dns->weight > lan->weight);
  const auto tunnel = std::find_if(rules.begin(), rules.end(), [](const auto& r) { return !r.localPrefixes.empty(); });
  CHECK(tunnel != rules.end() && tunnel->permit && tunnel->localPrefixes.size() == 1);

  // Without the local network: no LAN rules at all.
  const auto strict =
      BuildKillSwitchRules({.enabled = true, .allowLan = false, .lanClosed = false, .lanAllowed = {}}, tun);
  CHECK(std::none_of(strict.begin(), strict.end(), [](const auto& r) { return !r.remotePrefixes.empty(); }));
  CHECK(Count(strict, false) == 5);
}

// A rule of the closed local network's, by direction and name's end.
const KillSwitchRule* LanRule(const std::vector<KillSwitchRule>& rules, bool v6, bool inbound, const std::string& name) {
  const auto it = std::find_if(rules.begin(), rules.end(), [&](const auto& r) {
    return r.v6 == v6 && r.inbound == inbound && r.name.starts_with("LAN") && r.name.ends_with(name);
  });
  return it == rules.end() ? nullptr : &*it;
}

void TestLanClosed() {
  const auto tun = TunPrefixes(R"({"inbounds":[{"type":"tun","address":["172.19.0.1/30","fdfe:dcba:9876::1/126"]}]})");
  // Closed without the kill switch: only the local network's rules, both ways.
  const auto rules =
      BuildKillSwitchRules({.enabled = false,
                            .allowLan = true,
                            .lanClosed = true,
                            .lanAllowed = {"192.168.31.1", "nope", "fe80::1"}},
                           tun);
  CHECK(!rules.empty());
  CHECK(std::none_of(rules.begin(), rules.end(), [](const auto& r) { return r.coreApp; }));
  for (const bool v6 : {false, true}) {
    for (const bool inbound : {false, true}) {
      const KillSwitchRule* closed = LanRule(rules, v6, inbound, "closed");
      const KillSwitchRule* let = LanRule(rules, v6, inbound, "let in");
      const KillSwitchRule* tunnel = LanRule(rules, v6, inbound, "the tunnel");
      const KillSwitchRule* dhcp = LanRule(rules, v6, inbound, "DHCP");
      CHECK(closed != nullptr && !closed->permit && !closed->remotePrefixes.empty() && closed->coreApp == false);
      // What's let in, the tunnel (in a private range itself) and DHCP win over the block.
      CHECK(let != nullptr && let->permit && let->remotePrefixes.size() == 1 && let->weight > closed->weight);
      CHECK(tunnel != nullptr && tunnel->permit && tunnel->weight > closed->weight);
      CHECK(dhcp != nullptr && dhcp->permit && dhcp->weight > closed->weight);
      CHECK(inbound ? (dhcp->localPorts.size() == 1 && !dhcp->remotePort) : (dhcp->remotePort && dhcp->localPorts.empty()));
      // Above the kill switch's own rules (0..15): the core gets no way around it.
      CHECK(closed->weight > 15);
      // Not even an address let in answers names.
      const KillSwitchRule* dns = LanRule(rules, v6, inbound, "no DNS");
      CHECK(inbound ? dns == nullptr : (dns != nullptr && !dns->permit && dns->weight > let->weight));
      CHECK((LanRule(rules, v6, inbound, "neighbour discovery") != nullptr) == v6);
    }
  }

  // With the kill switch too: its rules under the closed network's, and its
  // LAN permit gone even with allowLan.
  const auto both =
      BuildKillSwitchRules({.enabled = true, .allowLan = true, .lanClosed = true, .lanAllowed = {}}, tun);
  const auto core = std::find_if(both.begin(), both.end(), [](const auto& r) { return r.coreApp; });
  CHECK(core != both.end() && core->weight < LanRule(both, false, false, "closed")->weight);
  CHECK(std::none_of(both.begin(), both.end(), [](const auto& r) { return r.name == "the local network"; }));
  CHECK(LanRule(both, false, false, "let in") == nullptr);  // nothing let in: no such rule
  CHECK(both.size() <= 64);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestPrefixes();
    TestTunPrefixes();
    TestRules();
    TestLanClosed();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
