#include "diagnostics.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <set>

namespace sovereign::tray {

namespace {

constexpr std::uint32_t kStunCookie = 0x2112A442;

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\r' || text.front() == '\n' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
  return out;
}

std::string Upper(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
  return out;
}

std::uint16_t Be16(std::string_view b, std::size_t at) {
  return static_cast<std::uint16_t>((static_cast<std::uint8_t>(b[at]) << 8) | static_cast<std::uint8_t>(b[at + 1]));
}

std::uint32_t Be32(std::string_view b, std::size_t at) {
  return (static_cast<std::uint32_t>(Be16(b, at)) << 16) | Be16(b, at + 2);
}

bool EndsWithLabel(std::string_view host, std::string_view suffix) {
  return host == suffix ||
         (host.size() > suffix.size() && host.ends_with(suffix) && host[host.size() - suffix.size() - 1] == '.');
}

}  // namespace

std::optional<Trace> ParseTrace(std::string_view body) {
  Trace trace;
  while (!body.empty()) {
    const std::size_t end = body.find('\n');
    const std::string_view line = Trim(body.substr(0, end));
    if (line.starts_with("ip=")) {
      trace.ip = std::string(line.substr(3));
    } else if (line.starts_with("loc=")) {
      trace.loc = std::string(line.substr(4));
    }
    if (end == std::string_view::npos) {
      break;
    }
    body.remove_prefix(end + 1);
  }
  if (!LooksLikeIp(trace.ip)) {
    return std::nullopt;
  }
  if (trace.loc.size() != 2 || !std::all_of(trace.loc.begin(), trace.loc.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) {
    trace.loc.clear();
  }
  return trace;
}

std::optional<std::string> ParseQuotedIp(std::string_view body) {
  body = Trim(body);
  if (body.size() >= 2 && body.front() == '"' && body.back() == '"') {
    body = body.substr(1, body.size() - 2);
  }
  if (!LooksLikeIp(body)) {
    return std::nullopt;
  }
  return std::string(body);
}

bool LooksLikeIp(std::string_view text) {
  if (text.empty() || text.size() > 45) {
    return false;
  }
  if (text.find(':') != std::string_view::npos) {
    return std::all_of(text.begin(), text.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.';
    });
  }
  int parts = 0;
  while (true) {
    const std::size_t dot = text.find('.');
    const std::string_view part = text.substr(0, dot);
    int value = -1;
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
    if (part.empty() || part.size() > 3 || error != std::errc{} || end != part.data() + part.size() || value > 255) {
      return false;
    }
    ++parts;
    if (dot == std::string_view::npos) {
      break;
    }
    text.remove_prefix(dot + 1);
  }
  return parts == 4;
}

std::string StunRequest(const StunId& id) {
  std::string message(20, '\0');
  message[1] = 0x01;  // Binding Request; length 0
  message[4] = static_cast<char>((kStunCookie >> 24) & 0xFF);
  message[5] = static_cast<char>((kStunCookie >> 16) & 0xFF);
  message[6] = static_cast<char>((kStunCookie >> 8) & 0xFF);
  message[7] = static_cast<char>(kStunCookie & 0xFF);
  for (std::size_t i = 0; i < id.size(); ++i) {
    message[8 + i] = static_cast<char>(id[i]);
  }
  return message;
}

std::optional<std::string> ParseStunResponse(std::string_view b, const StunId& id) {
  if (b.size() < 20 || Be16(b, 0) != 0x0101 || Be32(b, 4) != kStunCookie) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < id.size(); ++i) {
    if (static_cast<std::uint8_t>(b[8 + i]) != id[i]) {
      return std::nullopt;
    }
  }
  const std::size_t end = std::min<std::size_t>(b.size(), 20 + std::size_t{Be16(b, 2)});
  std::optional<std::string> mapped;
  for (std::size_t at = 20; at + 4 <= end;) {
    const std::uint16_t type = Be16(b, at);
    const std::uint16_t length = Be16(b, at + 2);
    const std::size_t value = at + 4;
    if (value + length > end) {
      break;
    }
    const bool xored = type == 0x0020;
    if ((xored || type == 0x0001) && length >= 8) {
      const auto family = static_cast<std::uint8_t>(b[value + 1]);
      if (family == 0x01) {
        std::uint32_t address = Be32(b, value + 4);
        if (xored) {
          address ^= kStunCookie;
        }
        std::string ip = std::format("{}.{}.{}.{}", (address >> 24) & 0xFF, (address >> 16) & 0xFF,
                                     (address >> 8) & 0xFF, address & 0xFF);
        if (xored) {
          return ip;  // XOR-MAPPED wins
        }
        mapped = std::move(ip);
      } else if (family == 0x02 && length >= 20) {
        std::array<std::uint8_t, 16> address{};
        for (std::size_t i = 0; i < 16; ++i) {
          address[i] = static_cast<std::uint8_t>(b[value + 4 + i]);
          if (xored) {
            address[i] ^= i < 4 ? static_cast<std::uint8_t>((kStunCookie >> (24 - 8 * i)) & 0xFF) : id[i - 4];
          }
        }
        std::string ip;
        for (std::size_t i = 0; i < 16; i += 2) {
          ip += std::format("{}{:x}", i == 0 ? "" : ":", (address[i] << 8) | address[i + 1]);
        }
        if (xored) {
          return ip;
        }
        mapped = std::move(ip);
      }
    }
    at = value + ((length + 3) & ~std::size_t{3});
  }
  return mapped;
}

std::vector<DnsResolverSeen> ParseDnsLeak(std::string_view json) {
  std::vector<DnsResolverSeen> seen;
  const auto list = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (!list.is_array()) {
    return seen;
  }
  const auto text = [](const nlohmann::json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  for (const auto& entry : list) {
    if (!entry.is_object() || text(entry, "type") != "dns") {
      continue;
    }
    DnsResolverSeen resolver{.ip = text(entry, "ip"), .country = Lower(text(entry, "country")), .org = text(entry, "org")};
    if (resolver.org.empty()) {
      resolver.org = text(entry, "asn");
    }
    if (LooksLikeIp(resolver.ip)) {
      seen.push_back(std::move(resolver));
    }
  }
  return seen;
}

CheckResult JudgeDnsLeak(const std::vector<DnsResolverSeen>& resolvers) {
  CheckResult result;
  if (resolvers.empty()) {
    result.status = CheckResult::Status::Warn;
    result.summary = "DNS-серверы не увиделись";
    result.detail = "Тестовые имена не дошли до сервиса проверки - попробуй ещё раз позже.";
    return result;
  }
  std::vector<std::string> names;
  std::set<std::string> seenNames;
  bool russian = false;
  for (const DnsResolverSeen& r : resolvers) {
    const std::string name = std::format("{} ({})", r.org.empty() ? r.ip : r.org, Upper(r.country));
    if (seenNames.insert(name).second) {
      names.push_back(name);
    }
    russian = russian || r.country == "ru";
  }
  std::string list;
  for (std::size_t i = 0; i < names.size() && i < 3; ++i) {
    list += (i > 0 ? ", " : "") + names[i];
  }
  if (names.size() > 3) {
    list += std::format(" и ещё {}", names.size() - 3);
  }
  result.summary = list;
  if (russian) {
    result.status = CheckResult::Status::Fail;
    result.detail = "Зарубежные имена спрашивает DNS-сервер в России - скорее всего, провайдера: он видит, куда ты ходишь.";
  } else {
    result.status = CheckResult::Status::Ok;
    result.detail = "Зарубежные имена спрашиваются через прокси - провайдер их не видит.";
  }
  return result;
}

std::optional<std::string> OutboundInLogs(const std::vector<std::string>& lines,
                                          const std::vector<std::string>& targets) {
  constexpr std::string_view kMarker = "outbound connection to ";
  for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
    const std::size_t at = line->find(kMarker);
    if (at == std::string::npos) {
      continue;
    }
    const std::string_view destination = std::string_view(*line).substr(at + kMarker.size());
    const bool ours = std::any_of(targets.begin(), targets.end(), [&](const std::string& t) {
      return !t.empty() && (destination.starts_with(t + ":") || destination.starts_with("[" + t + "]:"));
    });
    if (!ours) {
      continue;
    }
    const std::size_t out = line->rfind("outbound/", at);
    if (out == std::string::npos) {
      continue;
    }
    const std::string_view head = std::string_view(*line).substr(out + 9, at - out - 9);
    const std::size_t open = head.find('[');
    const std::size_t close = head.rfind(']');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
      continue;
    }
    if (head.substr(0, open) == "direct") {
      return std::string("direct");
    }
    return std::string(head.substr(open + 1, close - open - 1));
  }
  return std::nullopt;
}

std::string ClientRuleFor(std::string_view host, const RoutingSettings& routing) {
  const std::string name = Lower(Trim(host));
  for (const RouteRule& rule : routing.rules) {
    const bool domain = std::any_of(rule.domains.begin(), rule.domains.end(),
                                    [&](const std::string& d) { return EndsWithLabel(name, d); });
    const bool keyword = std::any_of(rule.keywords.begin(), rule.keywords.end(),
                                     [&](const std::string& k) { return name.find(k) != std::string::npos; });
    const bool ip = std::any_of(rule.ips.begin(), rule.ips.end(), [&](const std::string& cidr) {
      return cidr == name + "/32" || cidr == name + "/128";
    });
    if (domain || keyword || ip) {
      const char* action = rule.action == RouteRule::Action::Direct  ? "напрямую"
                           : rule.action == RouteRule::Action::Proxy ? "через прокси"
                                                                     : "блокировать";
      std::string text = RuleText(rule);
      if (text.size() > 40) {
        text = text.substr(0, 40) + "...";
      }
      return std::format("твоё правило «{}» → {}", text, action);
    }
  }
  if (routing.russiaDirect) {
    for (const std::string_view zone : {"ru", "su", "xn--p1ai", "xn--p1acf", "xn--80adxhks", "moscow", "tatar",
                                        "xn--d1acj3b", "xn--80asehdb", "xn--80aswg", "xn--c1avg"}) {
      if (EndsWithLabel(name, zone)) {
        return std::format("российская зона .{} → напрямую", zone);
      }
    }
  }
  return {};
}

double Mbps(std::uint64_t bytes, double seconds) {
  return seconds <= 0 ? 0 : static_cast<double>(bytes) * 8.0 / seconds / 1e6;
}

}  // namespace sovereign::tray
