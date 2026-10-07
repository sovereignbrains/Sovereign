#include "kill_switch.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <cstddef>
#include <iterator>
#include <utility>

namespace sovereign::service {

namespace {

std::optional<int> Number(std::string_view text, int base, int max) {
  if (text.empty() || text.size() > 5) {
    return std::nullopt;
  }
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (error != std::errc{} || end != text.data() + text.size() || value < 0 || value > max) {
    return std::nullopt;
  }
  return value;
}

bool ParseV4(std::string_view text, std::uint8_t* out) {
  for (int i = 0; i < 4; ++i) {
    const std::size_t dot = i < 3 ? text.find('.') : text.size();
    if (dot == std::string_view::npos) {
      return false;
    }
    const auto octet = Number(text.substr(0, dot), 10, 255);
    if (!octet || (dot > 1 && text[0] == '0')) {
      return false;
    }
    out[i] = static_cast<std::uint8_t>(*octet);
    text = i < 3 ? text.substr(dot + 1) : std::string_view();
  }
  return true;
}

// Hex groups with at most one "::"; no embedded IPv4 (TUN addresses don't use it).
bool ParseV6(std::string_view text, std::uint8_t* out) {
  std::array<std::uint16_t, 8> head{};
  std::array<std::uint16_t, 8> tail{};
  int heads = 0;
  int tails = 0;
  const std::size_t split = text.find("::");
  const auto groups = [](std::string_view part, std::array<std::uint16_t, 8>& into, int& count) {
    while (!part.empty()) {
      const std::size_t colon = part.find(':');
      const std::string_view group = part.substr(0, colon);
      const auto value = Number(group, 16, 0xFFFF);
      if (!value || group.size() > 4 || count == 8) {
        return false;
      }
      into[static_cast<std::size_t>(count++)] = static_cast<std::uint16_t>(*value);
      if (colon == std::string_view::npos) {
        break;
      }
      part = part.substr(colon + 1);
      if (part.empty()) {
        return false;  // a trailing single colon
      }
    }
    return true;
  };
  if (split != std::string_view::npos) {
    if (text.find("::", split + 1) != std::string_view::npos) {
      return false;
    }
    if (!groups(text.substr(0, split), head, heads) || !groups(text.substr(split + 2), tail, tails)) {
      return false;
    }
    if (heads + tails > 7) {
      return false;
    }
  } else if (!groups(text, head, heads) || heads != 8) {
    return false;
  }
  std::array<std::uint16_t, 8> all{};
  for (int i = 0; i < heads; ++i) {
    all[static_cast<std::size_t>(i)] = head[static_cast<std::size_t>(i)];
  }
  for (int i = 0; i < tails; ++i) {
    const int at = 8 - tails + i;
    all[static_cast<std::size_t>(at)] = tail[static_cast<std::size_t>(i)];
  }
  for (std::size_t i = 0; i < 8; ++i) {
    out[2 * i] = static_cast<std::uint8_t>(all[i] >> 8);
    out[2 * i + 1] = static_cast<std::uint8_t>(all[i] & 0xFF);
  }
  return true;
}

Prefix Make(bool v6, std::initializer_list<std::uint8_t> bytes, int bits) {
  Prefix p;
  p.v6 = v6;
  std::size_t i = 0;
  for (const std::uint8_t b : bytes) {
    p.bytes[i++] = b;
  }
  p.bits = bits;
  return p;
}

// Private, link-local and multicast destinations: the local network.
std::vector<Prefix> LanPrefixes(bool v6) {
  if (v6) {
    return {Make(true, {0xFE, 0x80}, 10), Make(true, {0xFC}, 7), Make(true, {0xFF}, 8)};
  }
  return {Make(false, {10}, 8),         Make(false, {172, 16}, 12),         Make(false, {192, 168}, 16),
          Make(false, {169, 254}, 16),  Make(false, {224}, 4),              Make(false, {255, 255, 255, 255}, 32)};
}

void AddAddresses(const nlohmann::json& value, std::vector<Prefix>& out) {
  const auto add = [&](const nlohmann::json& item) {
    if (item.is_string()) {
      if (auto prefix = ParsePrefix(item.get<std::string>())) {
        out.push_back(*prefix);
      }
    }
  };
  if (value.is_array()) {
    for (const auto& item : value) {
      add(item);
    }
  } else {
    add(value);
  }
}

}  // namespace

std::optional<Prefix> ParsePrefix(std::string_view text) {
  const std::size_t slash = text.find('/');
  const std::string_view address = text.substr(0, slash);
  Prefix prefix;
  prefix.v6 = address.find(':') != std::string_view::npos;
  if (prefix.v6 ? !ParseV6(address, prefix.bytes.data()) : !ParseV4(address, prefix.bytes.data())) {
    return std::nullopt;
  }
  const int max = prefix.v6 ? 128 : 32;
  prefix.bits = max;
  if (slash != std::string_view::npos) {
    const auto bits = Number(text.substr(slash + 1), 10, max);
    if (!bits) {
      return std::nullopt;
    }
    prefix.bits = *bits;
  }
  return prefix;
}

std::vector<Prefix> TunPrefixes(std::string_view config) {
  std::vector<Prefix> prefixes;
  const auto json = nlohmann::json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object() || !json.contains("inbounds") || !json["inbounds"].is_array()) {
    return prefixes;
  }
  for (const auto& inbound : json["inbounds"]) {
    if (!inbound.is_object() || inbound.value("type", nlohmann::json()) != "tun") {
      continue;
    }
    for (const char* key : {"address", "inet4_address", "inet6_address"}) {
      if (const auto it = inbound.find(key); it != inbound.end()) {
        AddAddresses(*it, prefixes);
      }
    }
  }
  return prefixes;
}

std::vector<KillSwitchRule> BuildKillSwitchRules(const KillSwitchSettings& settings, const std::vector<Prefix>& tun) {
  std::vector<KillSwitchRule> rules;
  if (!settings.enabled && !settings.lanClosed) {
    return rules;
  }
  for (const bool v6 : {false, true}) {
    const auto rule = [&](bool permit, std::uint8_t weight, std::string name) {
      KillSwitchRule r;
      r.v6 = v6;
      r.permit = permit;
      r.weight = weight;
      r.name = std::move(name);
      return r;
    };
    std::vector<Prefix> tunnelPrefixes;
    std::copy_if(tun.begin(), tun.end(), std::back_inserter(tunnelPrefixes), [&](const Prefix& p) { return p.v6 == v6; });

    // The local network closed: above everything of the kill switch's, the
    // core's permit included - 10..15 here, the kill switch 0..9. WFP takes
    // no more than 0..15 for a filter's FWP_UINT8 weight (FWP_E_INVALID_WEIGHT,
    // 0x80320025, past it).
    if (settings.lanClosed) {
      std::vector<Prefix> allowed;
      for (const std::string& text : settings.lanAllowed) {
        if (const auto p = ParsePrefix(text); p && p->v6 == v6 && allowed.size() < kMaxLanAllowed) {
          allowed.push_back(*p);
        }
      }
      for (const bool inbound : {false, true}) {
        const auto lanRule = [&](bool permit, std::uint8_t weight, std::string name) {
          KillSwitchRule r = rule(permit, weight, std::string(inbound ? "LAN in: " : "LAN out: ") + std::move(name));
          r.inbound = inbound;
          return r;
        };
        KillSwitchRule loopback = lanRule(true, 15, "loopback");
        loopback.loopback = true;
        rules.push_back(std::move(loopback));

        if (!tunnelPrefixes.empty()) {
          KillSwitchRule tunnel = lanRule(true, 14, "the tunnel");
          tunnel.localPrefixes = tunnelPrefixes;
          rules.push_back(std::move(tunnel));
        }

        if (!inbound) {
          KillSwitchRule dns = lanRule(false, 13, "no DNS");
          dns.remotePrefixes = LanPrefixes(v6);
          dns.remotePort = 53;
          rules.push_back(std::move(dns));
        }

        if (!allowed.empty()) {
          KillSwitchRule let = lanRule(true, 12, "let in");
          let.remotePrefixes = allowed;
          rules.push_back(std::move(let));
        }

        // DHCP: out to the server's port, back in to the client's.
        KillSwitchRule dhcp = lanRule(true, 11, "DHCP");
        dhcp.protocol = 17;
        if (inbound) {
          dhcp.localPorts = {static_cast<std::uint16_t>(v6 ? 546 : 68)};
        } else {
          dhcp.remotePort = v6 ? 547 : 67;
        }
        rules.push_back(std::move(dhcp));

        // IPv6 finds its router and neighbours over ICMPv6 (router
        // solicitation/advertisement, neighbour solicitation/advertisement,
        // redirect: types 133-137).
        if (v6) {
          KillSwitchRule ndp = lanRule(true, 11, "neighbour discovery");
          ndp.protocol = 58;
          ndp.localPorts = {133, 134, 135, 136, 137};
          rules.push_back(std::move(ndp));
        }

        KillSwitchRule closed = lanRule(false, 10, "closed");
        closed.remotePrefixes = LanPrefixes(v6);
        rules.push_back(std::move(closed));
      }
    }
    if (!settings.enabled) {
      continue;
    }

    KillSwitchRule core = rule(true, 9, "the core");
    core.coreApp = true;
    rules.push_back(std::move(core));

    KillSwitchRule loopback = rule(true, 8, "loopback");
    loopback.loopback = true;
    rules.push_back(std::move(loopback));

    if (!tunnelPrefixes.empty()) {
      KillSwitchRule tunnel = rule(true, 7, "the tunnel");
      tunnel.localPrefixes = tunnelPrefixes;
      rules.push_back(std::move(tunnel));
    }

    KillSwitchRule dhcp = rule(true, 6, "DHCP");
    dhcp.protocol = 17;
    dhcp.remotePort = v6 ? 547 : 67;
    rules.push_back(std::move(dhcp));

    if (settings.allowLan && !settings.lanClosed) {
      KillSwitchRule lanDns = rule(false, 5, "no DNS in the local network");
      lanDns.remotePrefixes = LanPrefixes(v6);
      lanDns.remotePort = 53;
      rules.push_back(std::move(lanDns));

      KillSwitchRule lan = rule(true, 4, "the local network");
      lan.remotePrefixes = LanPrefixes(v6);
      rules.push_back(std::move(lan));
    }

    rules.push_back(rule(false, 0, "everything else"));
  }
  return rules;
}

}  // namespace sovereign::service
