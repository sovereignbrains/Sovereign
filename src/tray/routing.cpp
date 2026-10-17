#include "routing.h"

#include "config_sync.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <initializer_list>
#include <set>
#include <utility>

namespace sovereign::tray {

namespace {

using Json = nlohmann::ordered_json;

// Russia's domain zones, in the form sing-box matches (punycode): ru, su, рф,
// рус, москва, moscow, tatar, дети, онлайн, сайт, орг.
constexpr std::array<std::string_view, 11> kRussianZones = {
    "ru", "su", "xn--p1ai", "xn--p1acf", "xn--80adxhks", "moscow", "tatar", "xn--d1acj3b", "xn--80asehdb",
    "xn--80aswg", "xn--c1avg"};

constexpr const char* kGeositeRu = "sov-geosite-ru";
constexpr const char* kGeositeGovRu = "sov-geosite-gov-ru";
constexpr const char* kGeoipRu = "sov-geoip-ru";
constexpr const char* kAds = "sov-ads";
// AdGuard DNS filter (~180 000 names: ads, trackers, Russian ones too) - the
// ads list beside sing-geosite's small one, made a rule-set here.
constexpr const char* kAdsAdGuard = "sov-ads-adguard";
constexpr std::string_view kAdGuardFilter = "https://adguardteam.github.io/HostlistsRegistry/assets/filter_1.txt";
// AdGuard Cookie Notices filter, a browser's: its ~190 whole names are the
// consent windows' scripts. "sov-ads" in the tag: gocore/blocked.go says what
// it blocked the way it does for the ads lists.
constexpr const char* kCookieWindows = "sov-ads-cookies";
constexpr std::string_view kCookieFilter = "https://filters.adtidy.org/extension/ublock/filters/18.txt";
constexpr const char* kLocalDns = "sov-local";
constexpr std::string_view kServicePrefix = "sov-svc-";  // a service's list: sov-svc-<id>

// A service's addresses' list (ServiceInfo::geoip): sov-svc-<id>-ip.
std::string ServiceIpsTag(std::string_view id) { return std::string(kServicePrefix) + std::string(id) + "-ip"; }

// What people want sent elsewhere: shut to a VPS's country (video, AI) or
// wanted apart, or wanted directly (games, the system's updates). Each list
// checked to exist in sing-geosite (06.10.2026, the second half 08.10.2026).
// Telegram's addresses: its app dials them, not names (MTProto, no name to
// sniff) - sing-geoip has no such list, MetaCubeX's meta-rules-dat has.
constexpr std::string_view kTelegramIps = "https://raw.githubusercontent.com/MetaCubeX/meta-rules-dat/sing/geo/geoip/telegram.srs";

// Where Steam and the Epic Games launcher put games: Steam in every library's
// steamapps\common, on any drive; Epic in an "Epic Games" folder (its default -
// moved when installing, a game isn't found).
constexpr std::string_view kSteamGames = R"((?i)\\steamapps\\common\\)";
constexpr std::string_view kEpicGames = R"((?i)\\Epic Games\\)";

constexpr std::array<ServiceInfo, 28> kServices = {{
    {"netflix", "Netflix", "netflix", {}, {}, {}, "netflix.com"},
    {"youtube", "YouTube", "youtube", {}, {}, {}, "www.youtube.com"},
    {"chatgpt", "ChatGPT", "openai", "ChatGPT.exe", {}, {}, "chatgpt.com"},
    {"gemini", "Gemini", "google-gemini", {}, {}, {}, "gemini.google.com"},
    {"claude", "Claude", "anthropic", {}, {}, {}, "claude.ai"},
    {"spotify", "Spotify", "spotify", "Spotify.exe", {}, {}, "open.spotify.com"},
    {"disney", "Disney+", "disney", {}, {}, {}, "www.disneyplus.com"},
    {"hbo", "HBO Max", "hbo", {}, {}, {}, "www.max.com"},
    {"primevideo", "Prime Video", "primevideo", {}, {}, {}, "www.primevideo.com"},
    {"twitch", "Twitch", "twitch", {}, {}, {}, "www.twitch.tv"},
    {"tiktok", "TikTok", "tiktok", {}, {}, {}, "www.tiktok.com"},
    {"instagram", "Instagram", "instagram", {}, {}, {}, "www.instagram.com"},
    {"facebook", "Facebook", "facebook", {}, {}, {}, "www.facebook.com"},
    {"x", "X (Twitter)", "twitter", {}, {}, {}, "x.com"},
    {"discord", "Discord", "discord", "Discord.exe", {}, {}, "discord.com"},
    {"telegram", "Telegram", "telegram", "Telegram.exe", kTelegramIps, {}, "web.telegram.org"},
    {"whatsapp", "WhatsApp", "whatsapp", "WhatsApp.exe,WhatsApp.Root.exe", {}, {}, "web.whatsapp.com"},
    {"google", "Google", "google", {}, {}, {}, "www.google.com"},
    {"perplexity", "Perplexity", "perplexity", "Perplexity.exe", {}, {}, "www.perplexity.ai"},
    {"grok", "Grok", "xai", {}, {}, {}, "grok.com"},
    {"github", "GitHub", "github", "GitHubDesktop.exe", {}, {}, "github.com"},
    {"reddit", "Reddit", "reddit", {}, {}, {}, "www.reddit.com"},
    {"linkedin", "LinkedIn", "linkedin", {}, {}, {}, "www.linkedin.com"},
    {"soundcloud", "SoundCloud", "soundcloud", {}, {}, {}, "soundcloud.com"},
    {"steam", "Steam", "steam", "steam.exe,steamwebhelper.exe", {}, kSteamGames, "store.steampowered.com"},
    {"epicgames", "Epic Games", "epicgames", "EpicGamesLauncher.exe,EpicWebHelper.exe", {}, kEpicGames, "store.epicgames.com"},
    {"microsoft", "Microsoft", "microsoft", {}, {}, {}, "www.microsoft.com"},
    {"apple", "Apple", "apple", {}, {}, {}, "www.apple.com"},
}};

constexpr std::string_view kGeositeBase = "https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set/";
constexpr std::string_view kGeoipBase = "https://raw.githubusercontent.com/SagerNet/sing-geoip/rule-set/";

// A blocked name answered "no addresses" (NOERROR, no records). Not REFUSED,
// not NXDOMAIN: Windows asks every adapter's DNS at once and, on a negative
// answer from the tunnel's, waits for the Wi-Fi's - which strict routing
// blocks - for ~11 s (measured 03.10.2026, both rcodes alike); an answer with
// no addresses it takes at once. Every blocked ad host used to stall
// whatever waited for it.
void SetNoAddress(Json& rule) {
  rule["action"] = "predefined";
  rule["rcode"] = "NOERROR";
}
std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
  return out;
}

std::string Str(const Json& object, const char* key) {
  if (!object.is_object()) {
    return {};
  }
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<std::string> Strings(const Json& value) {
  std::vector<std::string> out;
  if (value.is_string()) {
    out.push_back(value.get<std::string>());
  } else if (value.is_array()) {
    for (const Json& item : value) {
      if (item.is_string()) {
        out.push_back(item.get<std::string>());
      }
    }
  }
  return out;
}

// The own frame's DNS strategy: IPv4 only, or nothing when IPv6 is allowed.
const char* DnsStrategy(const RoutingSettings& s) { return s.ipv4Only ? "ipv4_only" : nullptr; }

const char* ActionName(RouteRule::Action action) {
  switch (action) {
    case RouteRule::Action::Direct: return "direct";
    case RouteRule::Action::Proxy: return "proxy";
    case RouteRule::Action::Block: return "block";
    case RouteRule::Action::Warp: return "warp";
  }
  return "direct";
}

const char* WayName(ServiceRoute::Way way) {
  switch (way) {
    case ServiceRoute::Way::Warp: return "warp";
    case ServiceRoute::Way::Country: return "country";
    case ServiceRoute::Way::Direct: return "direct";
    case ServiceRoute::Way::Proxy: return "proxy";
    case ServiceRoute::Way::Block: return "block";
  }
  return "warp";
}

bool IsIpToken(std::string_view token) {
  if (token.empty()) {
    return false;
  }
  const bool ipChars = std::all_of(token.begin(), token.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.' ||
           c == '/';
  });
  const bool v4 = std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.' || c == '/'; }) &&
                  std::count(token.begin(), token.end(), '.') == 3;
  const bool v6 = token.find(':') != std::string_view::npos;
  return ipChars && (v4 || v6);
}

bool IsDomainChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'; }

// The selector traffic goes through: the one route.final names, else the first.
std::string MainSelector(const Json& config) {
  std::string final = config.contains("route") ? Str(config["route"], "final") : std::string();
  std::string first;
  if (config.contains("outbounds") && config["outbounds"].is_array()) {
    for (const Json& o : config["outbounds"]) {
      if (Str(o, "type") != "selector") {
        continue;
      }
      if (!final.empty() && Str(o, "tag") == final) {
        return final;
      }
      if (first.empty()) {
        first = Str(o, "tag");
      }
    }
  }
  return first;
}

// The servers' own host names (not addresses), and the names their ECH
// configs are fetched under: sing-box asks the DNS router for both, and
// through the proxy a server's name would need that very server.
std::vector<std::string> ServerNames(const Json& config) {
  std::set<std::string> names;
  const auto add = [&](const std::string& host) {
    const bool letters = std::any_of(host.begin(), host.end(), [](char ch) { return std::isalpha(static_cast<unsigned char>(ch)) != 0; });
    if (letters && host.find(':') == std::string::npos) {
      names.insert(Lower(host));
    }
  };
  for (const char* list : {"outbounds", "endpoints"}) {
    if (!config.contains(list) || !config[list].is_array()) {
      continue;
    }
    for (const Json& o : config[list]) {
      if (!o.is_object()) {
        continue;
      }
      add(Str(o, "server"));
      if (const auto tls = o.find("tls"); tls != o.end() && tls->is_object()) {
        if (const auto ech = tls->find("ech"); ech != tls->end() && ech->is_object()) {
          add(Str(*ech, "query_server_name"));
        }
      }
      if (const auto peers = o.find("peers"); peers != o.end() && peers->is_array()) {
        for (const Json& peer : *peers) {
          add(Str(peer, "address"));
        }
      }
    }
  }
  return {names.begin(), names.end()};
}

// The servers behind a group, groups inside it expanded, in order, each once:
// the selector "proxy" -> "auto" and the servers -> the servers.
void GroupServers(const Json& outbounds, const std::string& tag, std::vector<std::string>& out, int depth = 0) {
  for (const Json& o : outbounds) {
    if (Str(o, "tag") != tag) {
      continue;
    }
    const std::string type = Str(o, "type");
    if (type != "selector" && type != "urltest") {
      if (type != "direct" && type != "block" && std::find(out.begin(), out.end(), tag) == out.end()) {
        out.push_back(tag);
      }
      return;
    }
    if (depth < 4 && o.contains("outbounds")) {
      for (const std::string& member : Strings(o["outbounds"])) {
        GroupServers(outbounds, member, out, depth + 1);
      }
    }
    return;
  }
}

// Whether every server behind `proxy` carries UDP as UDP - Hysteria, TUIC:
// QUIC goes through them as well as anything. Through any other (VLESS,
// Trojan, AnyTLS, Shadowsocks over TCP...) it'd be UDP packed in TCP.
bool ServersCarryQuic(const Json& outbounds, const std::string& proxy) {
  std::vector<std::string> servers;
  if (!proxy.empty()) {
    GroupServers(outbounds, proxy, servers);
  }
  return !servers.empty() && std::all_of(servers.begin(), servers.end(), [&](const std::string& tag) {
    return std::any_of(outbounds.begin(), outbounds.end(), [&](const Json& o) {
      const std::string type = Str(o, "type");
      return Str(o, "tag") == tag && (type == "hysteria" || type == "hysteria2" || type == "tuic");
    });
  });
}

bool BlockQuic(const RoutingSettings& s, const Json& config) {
  if (s.quic != RoutingSettings::Quic::Auto) {
    return s.quic == RoutingSettings::Quic::Block;
  }
  return !config.contains("outbounds") || !config["outbounds"].is_array() ||
         !ServersCarryQuic(config["outbounds"], MainSelector(config));
}

// Rules for the same things - names, keywords, addresses, programs - whatever
// their way; programs' names case-blind, as Windows has them.
bool SameThings(const RouteRule& a, const RouteRule& b) {
  return a.domains == b.domains && a.keywords == b.keywords && a.ips == b.ips &&
         std::equal(a.processes.begin(), a.processes.end(), b.processes.begin(), b.processes.end(),
                    [](const std::string& x, const std::string& y) { return Lower(x) == Lower(y); });
}

std::set<std::string> Tags(const Json& list) {
  std::set<std::string> tags;
  if (list.is_array()) {
    for (const Json& item : list) {
      if (const std::string tag = Str(item, "tag"); !tag.empty()) {
        tags.insert(tag);
      }
    }
  }
  return tags;
}

// The system's resolver, for the name of a DNS server the user typed by name.
constexpr const char* kBootstrapDns = "sov-bootstrap";

// A typed DNS server as sing-box takes it, tagged `tag`.
Json CustomDnsServer(const CustomDns& dns, const std::string& tag) {
  Json server{{"type", dns.type}, {"tag", tag}, {"server", dns.host}, {"server_port", dns.port}};
  if (dns.type == "https") {
    server["path"] = dns.path;
  }
  if (dns.named) {
    server["domain_resolver"] = kBootstrapDns;
  }
  return server;
}

// The local server: an encrypted one by its address, the system's, or the
// user's own (one that doesn't parse: Cloudflare's, as Auto).
Json LocalDnsServer(RoutingSettings::LocalDns local, const std::string& custom) {
  if (local == RoutingSettings::LocalDns::Custom) {
    if (const auto dns = ParseCustomDns(custom)) {
      return CustomDnsServer(*dns, kLocalDns);
    }
    local = RoutingSettings::LocalDns::Auto;
  }
  if (const std::string_view address = LocalDnsAddress(local); !address.empty()) {
    return Json{{"type", "https"}, {"tag", kLocalDns}, {"server", std::string(address)}};
  }
  return Json{{"type", "local"}, {"tag", kLocalDns}};
}

// The servers a name of which is resolved first get the system's resolver
// to do it.
void AddBootstrapDns(Json& servers) {
  const bool wanted = std::any_of(servers.begin(), servers.end(), [](const Json& s) {
    return s.is_object() && Str(s, "domain_resolver") == kBootstrapDns;
  });
  if (wanted && !Tags(servers).contains(kBootstrapDns)) {
    servers.push_back(Json{{"type", "local"}, {"tag", kBootstrapDns}});
  }
}

// `items` into `list` at `at`.
void Prepend(Json& list, std::vector<Json> items, std::size_t at) {
  Json out = Json::array();
  for (std::size_t i = 0; i < at && i < list.size(); ++i) {
    out.push_back(std::move(list[i]));
  }
  for (Json& item : items) {
    out.push_back(std::move(item));
  }
  for (std::size_t i = at; i < list.size(); ++i) {
    out.push_back(std::move(list[i]));
  }
  list = std::move(out);
}

// A DNS rule sing-box reads the old way (an address filter, a strategy) - it
// can't sit next to evaluate; nor one already using answers. A rule set can't
// be told from here (one of addresses is a filter too): the core decides, and
// the tray builds the config again without the layout if it's refused.
bool OldStyleDnsRule(const Json& rule) {
  static constexpr std::array kKeys = {"ip_cidr",        "ip_is_private",   "ip_accept_any", "rule_set_ip_cidr_accept_empty",
                                       "rule_set_ipcidr_accept_empty", "match_response", "response_rcode",
                                       "response_answer", "response_ns",     "response_extra", "race", "strategy"};
  if (!rule.is_object()) {
    return true;
  }
  for (const char* key : kKeys) {
    if (rule.contains(key)) {
      return true;
    }
  }
  if (const std::string action = Str(rule, "action"); action == "evaluate" || action == "respond") {
    return true;
  }
  if (rule.contains("rules")) {
    for (const Json& sub : rule.at("rules")) {
      if (OldStyleDnsRule(sub)) {
        return true;
      }
    }
  }
  return false;
}

// Typos answered "no addresses" too: their NXDOMAIN kept Windows waiting
// ~11 s just as a blocked name's did (see SetNoAddress). Only the answer tells
// a typo, so a rule sending names to a server becomes three: ask it
// (evaluate), NXDOMAIN -> no addresses, else its answer (respond under the
// same condition: a failed query fails, as the route's did, instead of falling
// to later rules). The final server gets the three at the end. Fakeip routes
// stay - evaluate can't use them, and they never answer NXDOMAIN.
void AnswerTyposAtOnce(Json& dns) {
  static constexpr std::array kActionKeys = {"action",        "server",  "disable_cache", "disable_optimistic_cache",
                                             "rewrite_ttl",   "timeout", "client_subnet", "remove_client_subnet",
                                             "speculative"};
  Json& rules = dns["rules"];
  std::set<std::string> fakeip;
  std::string final = Str(dns, "final");
  for (const Json& server : dns["servers"]) {
    if (Str(server, "type") == "fakeip") {
      fakeip.insert(Str(server, "tag"));
    }
    if (final.empty()) {
      final = Str(server, "tag");
    }
  }
  if (final.empty() || fakeip.contains(final) || std::ranges::any_of(rules, OldStyleDnsRule)) {
    return;
  }
  Json out = Json::array();
  const auto ask = [&out](Json evaluate) {
    Json respond = evaluate;
    for (const char* key : kActionKeys) {
      respond.erase(key);
    }
    respond["action"] = "respond";
    evaluate["action"] = "evaluate";
    Json typo{{"match_response", true}, {"response_rcode", "NXDOMAIN"}};
    SetNoAddress(typo);
    out.push_back(std::move(evaluate));
    out.push_back(std::move(typo));
    out.push_back(std::move(respond));
  };
  for (Json& rule : rules) {
    if (const std::string action = Str(rule, "action");
        (action.empty() || action == "route") && !fakeip.contains(Str(rule, "server"))) {
      ask(std::move(rule));
    } else {
      out.push_back(std::move(rule));
    }
  }
  ask(Json{{"server", final}});
  rules = std::move(out);
}

}  // namespace

nlohmann::json RoutingToJson(const RoutingSettings& s) {
  nlohmann::json rules = nlohmann::json::array();
  for (const RouteRule& r : s.rules) {
    nlohmann::json rule = {{"domains", r.domains},
                           {"keywords", r.keywords},
                           {"ips", r.ips},
                           {"processes", r.processes},
                           {"action", ActionName(r.action)}};
    if (!r.name.empty()) {
      rule["name"] = r.name;
    }
    rules.push_back(std::move(rule));
  }
  nlohmann::json services = nlohmann::json::array();
  for (const ServiceRoute& r : s.services) {
    nlohmann::json service = {{"id", r.id}, {"way", WayName(r.way)}};
    if (r.way == ServiceRoute::Way::Country) {
      service["country"] = r.country;
    }
    services.push_back(std::move(service));
  }
  return {{"russiaDirect", s.russiaDirect},
          {"blockAds", s.blockAds},
          {"cookieWindows", s.cookieWindows},
          {"quic", s.quic == RoutingSettings::Quic::Block   ? "block"
                   : s.quic == RoutingSettings::Quic::Allow ? "allow"
                                                            : "auto"},
          {"hysteriaMbps", s.hysteriaMbps},
          {"finalDirect", s.finalDirect},
          {"remoteDns", s.remoteDns == RoutingSettings::RemoteDns::Google     ? "google"
                        : s.remoteDns == RoutingSettings::RemoteDns::Quad9    ? "quad9"
                        : s.remoteDns == RoutingSettings::RemoteDns::Auto     ? "auto"
                        : s.remoteDns == RoutingSettings::RemoteDns::Custom   ? "custom"
                                                                               : "cloudflare"},
          {"remoteDnsCustom", s.remoteDnsCustom},
          {"localDns", s.localDns == RoutingSettings::LocalDns::Google   ? "google"
                       : s.localDns == RoutingSettings::LocalDns::System ? "system"
                       : s.localDns == RoutingSettings::LocalDns::Quad9  ? "quad9"
                       : s.localDns == RoutingSettings::LocalDns::Auto   ? "auto"
                       : s.localDns == RoutingSettings::LocalDns::Custom ? "custom"
                                                                          : "cloudflare"},
          {"localDnsCustom", s.localDnsCustom},
          {"ipv4Only", s.ipv4Only},
          {"rules", std::move(rules)},
          {"services", std::move(services)},
          {"warp", s.warp},
          {"warpViaProxy", s.warpViaProxy},
              {"warpAccount", s.warpAccount ? WarpToJson(*s.warpAccount) : nlohmann::json()}};
}

RoutingSettings RoutingFromJson(const nlohmann::json& json) {
  RoutingSettings s;
  if (!json.is_object()) {
    return s;
  }
  const auto flag = [&](const char* key, bool& into) {
    if (const auto it = json.find(key); it != json.end() && it->is_boolean()) {
      into = it->get<bool>();
    }
  };
  const auto text = [&](const char* key) {
    const auto it = json.find(key);
    return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  flag("russiaDirect", s.russiaDirect);
  flag("blockAds", s.blockAds);
  flag("cookieWindows", s.cookieWindows);
  // "blockQuic" before 0.4.56: off stays off; on, the default, is auto now.
  bool blockQuic = true;
  flag("blockQuic", blockQuic);
  s.quic = blockQuic ? RoutingSettings::Quic::Auto : RoutingSettings::Quic::Allow;
  if (const std::string quic = text("quic"); quic == "auto" || quic == "block" || quic == "allow") {
    s.quic = quic == "block"   ? RoutingSettings::Quic::Block
             : quic == "allow" ? RoutingSettings::Quic::Allow
                               : RoutingSettings::Quic::Auto;
  }
  if (const auto it = json.find("hysteriaMbps"); it != json.end() && it->is_number_integer()) {
    s.hysteriaMbps = static_cast<int>(std::clamp<std::int64_t>(it->get<std::int64_t>(), 0, 10000));
  }
  flag("finalDirect", s.finalDirect);
  flag("ipv4Only", s.ipv4Only);
  if (const auto it = json.find("services"); it != json.end() && it->is_array()) {
    for (const nlohmann::json& item : *it) {
      const std::string id = item.is_object() ? item.value("id", std::string()) : std::string();
      const bool known = FindService(id) != nullptr;
      const bool twice = std::any_of(s.services.begin(), s.services.end(), [&](const ServiceRoute& r) { return r.id == id; });
      if (known && !twice) {
        const std::string way = item.value("way", std::string());
        const std::string country = item.value("country", std::string());
        if (way == "country" && IsCountryCode(country)) {
          s.services.push_back({.id = id, .way = ServiceRoute::Way::Country, .country = country});
        } else {
          // "country" without one, or a way unknown: through WARP, as before 0.4.50.
          const ServiceRoute::Way parsed = way == "direct"  ? ServiceRoute::Way::Direct
                                           : way == "proxy" ? ServiceRoute::Way::Proxy
                                           : way == "block" ? ServiceRoute::Way::Block
                                                            : ServiceRoute::Way::Warp;
          s.services.push_back({.id = id, .way = parsed, .country = {}});
        }
      }
    }
  }
  flag("warp", s.warp);
  flag("warpViaProxy", s.warpViaProxy);
  if (const auto it = json.find("warpAccount"); it != json.end()) {
    s.warpAccount = WarpFromJson(*it);
  }
  // A typed address kept only while it parses (300: a URL, not a page).
  const auto custom = [&](const char* key) {
    std::string value = text(key).substr(0, 300);
    return ParseCustomDns(value) ? value : std::string();
  };
  s.remoteDnsCustom = custom("remoteDnsCustom");
  s.localDnsCustom = custom("localDnsCustom");
  // Nothing said (before 0.4.84, or a new install): auto.
  const std::string remote = text("remoteDns");
  s.remoteDns = remote == "google"       ? RoutingSettings::RemoteDns::Google
                : remote == "quad9"      ? RoutingSettings::RemoteDns::Quad9
                : remote == "cloudflare" ? RoutingSettings::RemoteDns::Cloudflare
                : remote == "custom" && !s.remoteDnsCustom.empty() ? RoutingSettings::RemoteDns::Custom
                                                                    : RoutingSettings::RemoteDns::Auto;
  const std::string local = text("localDns");
  s.localDns = local == "google"       ? RoutingSettings::LocalDns::Google
               : local == "system"     ? RoutingSettings::LocalDns::System
               : local == "quad9"      ? RoutingSettings::LocalDns::Quad9
               : local == "cloudflare" ? RoutingSettings::LocalDns::Cloudflare
               : local == "custom" && !s.localDnsCustom.empty() ? RoutingSettings::LocalDns::Custom
                                                                 : RoutingSettings::LocalDns::Auto;
  if (const auto it = json.find("rules"); it != json.end() && it->is_array()) {
    for (const auto& entry : *it) {
      if (!entry.is_object()) {
        continue;
      }
      // Names, keywords and addresses through the text (it cleans them);
      // programs as they are, in quotes - a name with a space in it
      // ("SSH Client.exe") came back as a site and a program before 0.4.68.
      std::string line;
      for (const char* key : {"domains", "keywords", "ips", "processes"}) {
        if (const auto values = entry.find(key); values != entry.end() && values->is_array()) {
          for (const auto& v : *values) {
            if (!v.is_string()) {
              continue;
            }
            const std::string value = v.get<std::string>();
            if (std::string_view(key) == "processes") {
              if (value.find('"') == std::string::npos) {
                line += "\"" + value + "\" ";
              }
            } else {
              // Keywords keep their kind through the text: "~word".
              line += (std::string_view(key) == "keywords" ? "~" : "") + value + " ";
            }
          }
        }
      }
      const std::string action = entry.contains("action") && entry["action"].is_string() ? entry["action"].get<std::string>() : "";
      auto rule = ParseRule(line, action == "block"   ? RouteRule::Action::Block
                                        : action == "proxy" ? RouteRule::Action::Proxy
                                        : action == "warp"  ? RouteRule::Action::Warp
                                                            : RouteRule::Action::Direct);
      // One the same as an earlier one (in two buckets, before 0.4.57): only
      // the first ever worked - the rest dropped.
      if (rule && std::none_of(s.rules.begin(), s.rules.end(), [&](const RouteRule& r) { return SameThings(r, *rule); })) {
        if (const auto name = entry.find("name"); name != entry.end() && name->is_string()) {
          rule->name = CleanRuleName(name->get<std::string>());
        }
        s.rules.push_back(*rule);
      }
    }
  }
  return s;
}

namespace {

// An exe's name, a space in it or not - no path, nothing a file name can't hold.
bool IsProgramName(std::string_view t) {
  return t.size() > 4 && Lower(t).ends_with(".exe") && t.find_first_of("\\/:*?\"<>|") == std::string_view::npos &&
         t.front() != ' ';
}

}  // namespace

std::optional<RouteRule> ProgramRule(std::string_view exe, RouteRule::Action action) {
  if (!IsProgramName(exe) || exe.size() > 260) {
    return std::nullopt;
  }
  RouteRule rule;
  rule.action = action;
  rule.processes.emplace_back(exe);
  return rule;
}

std::optional<RouteRule> ParseRule(std::string_view text, RouteRule::Action action) {
  RouteRule rule;
  rule.action = action;
  std::string token;
  const auto take = [&] {
    if (token.empty()) {
      return;
    }
    std::string t = std::move(token);
    token.clear();
    const auto add = [](std::vector<std::string>& into, std::string value) {
      if (!value.empty() && std::find(into.begin(), into.end(), value) == into.end() && into.size() < 512) {
        into.push_back(std::move(value));
      }
    };
    if (t.size() > 1 && t.front() == '~') {
      std::string keyword = Lower(t.substr(1));
      if (std::all_of(keyword.begin(), keyword.end(), IsDomainChar)) {
        add(rule.keywords, std::move(keyword));
      }
      return;
    }
    if (IsProgramName(t)) {
      add(rule.processes, std::move(t));
      return;
    }
    if (IsIpToken(t)) {
      add(rule.ips, t.find('/') != std::string::npos ? t : t + (t.find(':') != std::string::npos ? "/128" : "/32"));
      return;
    }
    // A name: "https://www.example.com/path", "*.example.com", ".example.com".
    std::string name = Lower(t);
    if (const std::size_t scheme = name.find("://"); scheme != std::string::npos) {
      name = name.substr(scheme + 3);
    }
    name = name.substr(0, name.find_first_of("/?#:"));
    while (name.starts_with("*.") || name.starts_with(".")) {
      name.erase(0, name.starts_with("*.") ? 2 : 1);
    }
    while (name.ends_with(".")) {
      name.pop_back();
    }
    if (!name.empty() && name.size() <= 253 && std::all_of(name.begin(), name.end(), IsDomainChar)) {
      add(rule.domains, std::move(name));
    }
  };
  // In quotes a space is part of the token: "SSH Client.exe".
  bool quoted = false;
  for (const char c : text) {
    if (c == '"') {
      quoted = !quoted;
    } else if (!quoted && (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
      take();
    } else if (c != '\r' && c != '\n' && token.size() < 300) {
      token.push_back(c);
    }
  }
  take();
  if (rule.domains.empty() && rule.keywords.empty() && rule.ips.empty() && rule.processes.empty()) {
    return std::nullopt;
  }
  return rule;
}

std::string RuleText(const RouteRule& rule) {
  std::string text;
  const auto add = [&](const std::vector<std::string>& values, std::string_view prefix) {
    for (const std::string& v : values) {
      text += (text.empty() ? "" : ", ") + std::string(prefix) + v;
    }
  };
  add(rule.domains, "");
  add(rule.keywords, "~");
  add(rule.ips, "");
  add(rule.processes, "");
  return text;
}

std::vector<RouteRule> ImportRules(std::string_view config) {
  std::vector<RouteRule> rules;
  if (NestingDepth(config) > kMaxConfigDepth) {
    return rules;
  }
  const Json json = Json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object() || !json.contains("route") || !json["route"].is_object() ||
      !json["route"].contains("rules") || !json["route"]["rules"].is_array()) {
    return rules;
  }
  std::set<std::string> direct;
  std::set<std::string> proxies;
  if (json.contains("outbounds") && json["outbounds"].is_array()) {
    for (const Json& o : json["outbounds"]) {
      (Str(o, "type") == "direct" ? direct : proxies).insert(Str(o, "tag"));
    }
  }
  static const std::set<std::string> kKnown = {"domain", "domain_suffix", "domain_keyword", "ip_cidr",
                                               "process_name", "outbound", "action"};
  for (const Json& r : json["route"]["rules"]) {
    if (!r.is_object()) {
      continue;
    }
    bool simple = true;
    for (auto it = r.begin(); it != r.end(); ++it) {
      simple = simple && kKnown.contains(it.key());
    }
    // A program and sites together match both at once there; here a rule
    // matches either - not the same rule, left out.
    const bool sitesToo = r.contains("domain") || r.contains("domain_suffix") || r.contains("domain_keyword") ||
                          r.contains("ip_cidr");
    if (!simple || (r.contains("process_name") && sitesToo)) {
      continue;
    }
    const std::string action = Str(r, "action");
    const std::string outbound = Str(r, "outbound");
    RouteRule::Action kind{};
    if (action == "reject") {
      kind = RouteRule::Action::Block;
    } else if ((action.empty() || action == "route") && direct.contains(outbound)) {
      kind = RouteRule::Action::Direct;
    } else if ((action.empty() || action == "route") && proxies.contains(outbound)) {
      kind = RouteRule::Action::Proxy;
    } else {
      continue;
    }
    std::string line;
    for (const char* key : {"domain", "domain_suffix", "ip_cidr", "process_name"}) {
      if (r.contains(key)) {
        for (const std::string& v : Strings(r[key])) {
          line += v + " ";
        }
      }
    }
    if (r.contains("domain_keyword")) {
      for (const std::string& v : Strings(r["domain_keyword"])) {
        line += "~" + v + " ";
      }
    }
    if (auto rule = ParseRule(line, kind)) {
      rules.push_back(std::move(*rule));
    }
  }
  return rules;
}

void MergeRules(std::vector<RouteRule>& into, const std::vector<RouteRule>& more) {
  for (const RouteRule& rule : more) {
    const bool known = std::any_of(into.begin(), into.end(), [&](const RouteRule& r) {
      return r.action == rule.action && r.domains == rule.domains && r.keywords == rule.keywords && r.ips == rule.ips &&
             r.processes == rule.processes;
    });
    if (!known) {
      into.push_back(rule);
    }
  }
}

void AddRule(std::vector<RouteRule>& rules, const RouteRule& rule) {
  std::erase_if(rules, [&](const RouteRule& r) { return SameThings(r, rule); });
  rules.push_back(rule);
}

std::string RuleInput(const RouteRule& rule) {
  RouteRule quoted = rule;
  for (std::string& p : quoted.processes) {
    if (p.find(' ') != std::string::npos) {
      p.insert(p.begin(), '"');
      p += '"';
    }
  }
  return RuleText(quoted);
}

bool EditRule(std::vector<RouteRule>& rules, std::size_t index, std::string_view text) {
  if (index >= rules.size()) {
    return false;
  }
  auto parsed = ParseRule(text, rules[index].action);
  if (!parsed) {
    return false;
  }
  parsed->name = rules[index].name;
  rules[index] = *parsed;
  const RouteRule& edited = *parsed;  // not rules[index]: the ones before it may go
  for (std::size_t i = rules.size(); i-- > 0;) {
    if (i != index && SameThings(rules[i], edited)) {
      rules.erase(rules.begin() + static_cast<std::ptrdiff_t>(i));
      index -= i < index ? 1 : 0;
    }
  }
  return true;
}

bool AddProgramToRule(std::vector<RouteRule>& rules, std::size_t index, std::string_view exe) {
  const auto alone = ProgramRule(exe, RouteRule::Action::Direct);
  if (!alone || index >= rules.size()) {
    return false;
  }
  std::vector<std::string>& processes = rules[index].processes;
  if (std::none_of(processes.begin(), processes.end(), [&](const std::string& p) { return Lower(p) == Lower(exe); })) {
    processes.emplace_back(exe);
  }
  for (std::size_t i = rules.size(); i-- > 0;) {
    if (i != index && SameThings(rules[i], *alone)) {
      rules.erase(rules.begin() + static_cast<std::ptrdiff_t>(i));
      index -= i < index ? 1 : 0;
    }
  }
  return true;
}

std::string CleanRuleName(std::string_view name) {
  std::string out;
  std::size_t characters = 0;
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      continue;  // one line
    }
    const bool starts = (byte & 0xC0) != 0x80;  // not a UTF-8 continuation byte
    if (starts && ++characters > 40) {
      break;
    }
    out += c;
  }
  const std::size_t first = out.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

std::span<const ServiceInfo> ServiceCatalog() { return kServices; }

std::string ServiceOutboundTag(std::string_view id) { return "sov-out-" + std::string(id); }

bool IsCountryCode(std::string_view code) {
  return code.size() == 2 && std::all_of(code.begin(), code.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
}

std::string PickCountryServer(std::string_view country, const std::vector<std::string>& servers,
                              const std::map<std::string, std::string>& exitCountry,
                              const std::map<std::string, int>& delayMs) {
  std::string best;
  int bestDelay = 0;
  for (const std::string& server : servers) {
    const auto exit = exitCountry.find(server);
    if (exit == exitCountry.end() || exit->second != country) {
      continue;
    }
    const auto delay = delayMs.find(server);
    const int ms = delay != delayMs.end() && delay->second > 0 ? delay->second : 0;
    // Measured beats not measured; among measured, the faster; else the first.
    if (best.empty() || (ms > 0 && (bestDelay == 0 || ms < bestDelay))) {
      best = server;
      bestDelay = ms;
    }
  }
  return best;
}

std::string CountryName(std::string_view code) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 44> kNames = {{
      {"US", "США"},          {"NL", "Нидерланды"},  {"DE", "Германия"},   {"GB", "Великобритания"},
      {"FR", "Франция"},      {"FI", "Финляндия"},   {"SE", "Швеция"},     {"NO", "Норвегия"},
      {"DK", "Дания"},        {"PL", "Польша"},      {"LV", "Латвия"},     {"LT", "Литва"},
      {"EE", "Эстония"},      {"CH", "Швейцария"},   {"AT", "Австрия"},    {"IT", "Италия"},
      {"ES", "Испания"},      {"PT", "Португалия"},  {"IE", "Ирландия"},   {"BE", "Бельгия"},
      {"CZ", "Чехия"},        {"RO", "Румыния"},     {"BG", "Болгария"},   {"HU", "Венгрия"},
      {"GR", "Греция"},       {"CY", "Кипр"},        {"MD", "Молдова"},    {"RS", "Сербия"},
      {"AL", "Албания"},      {"TR", "Турция"},      {"GE", "Грузия"},     {"AM", "Армения"},
      {"KZ", "Казахстан"},    {"AE", "ОАЭ"},         {"IL", "Израиль"},    {"JP", "Япония"},
      {"KR", "Корея"},        {"SG", "Сингапур"},    {"HK", "Гонконг"},    {"IN", "Индия"},
      {"CA", "Канада"},       {"AU", "Австралия"},   {"BR", "Бразилия"},   {"RU", "Россия"},
  }};
  for (const auto& [c, name] : kNames) {
    if (c == code) {
      return std::string(name);
    }
  }
  return std::string(code);
}

std::string CountryNameVia(std::string_view code) {
  std::string name = CountryName(code);
  // The feminine names end in -я or -а, and take -ю or -у; the rest - США,
  // Нидерланды, Кипр, Казахстан - stay.
  for (const auto& [from, to] : {std::pair<std::string_view, std::string_view>{"я", "ю"}, {"а", "у"}}) {
    if (name.ends_with(from)) {
      name.replace(name.size() - from.size(), from.size(), to);
      break;
    }
  }
  return name;
}

const ServiceInfo* FindService(std::string_view id) {
  const auto it = std::find_if(kServices.begin(), kServices.end(), [&](const ServiceInfo& s) { return s.id == id; });
  return it == kServices.end() ? nullptr : &*it;
}

std::vector<std::string> ServiceApps(const ServiceInfo& service) {
  std::vector<std::string> apps;
  for (std::string_view rest = service.apps; !rest.empty();) {
    const std::size_t comma = std::min(rest.find(','), rest.size());
    if (comma > 0) {
      apps.emplace_back(rest.substr(0, comma));
    }
    rest.remove_prefix(std::min(comma + 1, rest.size()));
  }
  return apps;
}

const ServiceInfo* ServiceOfProgram(std::string_view exe) {
  std::string name = Lower(exe);
  if (!name.ends_with(".exe")) {
    return nullptr;
  }
  for (const ServiceInfo& s : kServices) {
    for (const std::string& app : ServiceApps(s)) {
      if (Lower(app) == name) {
        return &s;
      }
    }
  }
  name.resize(name.size() - 4);
  // The name's letters and digits alone: "Epic Games" would be epicgames.exe.
  const auto plain = [](std::string_view text) {
    std::string out;
    for (const char c : Lower(text)) {
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        out += c;
      }
    }
    return out;
  };
  const auto it = std::find_if(kServices.begin(), kServices.end(),
                               [&](const ServiceInfo& s) { return name == s.id || name == plain(s.name); });
  return it == kServices.end() ? nullptr : &*it;
}

std::optional<std::string> AdGuardRuleSet(std::string_view filter) {
  std::vector<std::string> blocked;
  std::vector<std::string> allowed;
  while (!filter.empty()) {
    const std::size_t end = filter.find('\n');
    std::string_view line = filter.substr(0, end);
    filter = end == std::string_view::npos ? std::string_view() : filter.substr(end + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.remove_suffix(1);
    }
    const bool allow = line.starts_with("@@");
    if (allow) {
      line.remove_prefix(2);
    }
    if (!line.starts_with("||")) {
      continue;
    }
    line.remove_prefix(2);
    for (const std::string_view modifier : {"$important", "$third-party", "$3p"}) {
      if (line.ends_with(modifier)) {
        line.remove_suffix(modifier.size());
        break;
      }
    }
    if (line.ends_with('|')) {
      line.remove_suffix(1);
    }
    if (!line.ends_with('^')) {
      continue;  // other modifiers ($badfilter...), a path
    }
    line.remove_suffix(1);
    const std::string name = Lower(line);
    const bool plain = !name.empty() && name.front() != '.' && name.back() != '.' &&
                       name.find("..") == std::string::npos && std::all_of(name.begin(), name.end(), IsDomainChar);
    if (plain) {
      (allow ? allowed : blocked).push_back(name);
    }
  }
  if (blocked.empty()) {
    return std::nullopt;
  }
  const Json names{{"domain_suffix", blocked}};
  const Json rule = allowed.empty() ? names
                                    : Json{{"type", "logical"},
                                           {"mode", "and"},
                                           {"rules", Json::array({names, Json{{"domain_suffix", allowed}, {"invert", true}}})}};
  return Json{{"version", 3}, {"rules", Json::array({rule})}}.dump();
}

std::vector<RuleSetSource> NeededRuleSets(const RoutingSettings& settings) {
  std::vector<RuleSetSource> sets;
  if (settings.russiaDirect) {
    sets.push_back({std::string(kGeositeRu), std::string(kGeositeBase) + "geosite-category-ru.srs", false});
    sets.push_back({std::string(kGeositeGovRu), std::string(kGeositeBase) + "geosite-category-gov-ru.srs", false});
    sets.push_back({std::string(kGeoipRu), std::string(kGeoipBase) + "geoip-ru.srs", true});
  }
  if (settings.blockAds) {
    sets.push_back({std::string(kAds), std::string(kGeositeBase) + "geosite-category-ads-all.srs", false});
    sets.push_back({std::string(kAdsAdGuard), std::string(kAdGuardFilter), false, true});
  }
  if (settings.cookieWindows) {
    sets.push_back({std::string(kCookieWindows), std::string(kCookieFilter), false, true});
  }
  for (const ServiceRoute& r : settings.services) {
    if (const ServiceInfo* info = FindService(r.id)) {
      sets.push_back({std::string(kServicePrefix) + r.id,
                      std::string(kGeositeBase) + "geosite-" + std::string(info->geosite) + ".srs", false});
      if (!info->geoip.empty()) {
        sets.push_back({ServiceIpsTag(r.id), std::string(info->geoip), true});
      }
    }
  }
  return sets;
}

std::string OwnFrame(const RoutingSettings& settings) {
  const char* address = settings.remoteDns == RoutingSettings::RemoteDns::Google  ? "8.8.8.8"
                        : settings.remoteDns == RoutingSettings::RemoteDns::Quad9 ? "9.9.9.9"
                                                                                   : "1.1.1.1";
  Json remote{{"type", "https"}, {"tag", "remote"}, {"server", address}};
  if (settings.remoteDns == RoutingSettings::RemoteDns::Custom) {
    if (const auto dns = ParseCustomDns(settings.remoteDnsCustom)) {
      remote = CustomDnsServer(*dns, "remote");
    }
  }
  remote["detour"] = "proxy";
  Json dns = {{"servers", Json::array({std::move(remote), LocalDnsServer(settings.localDns, settings.localDnsCustom)})},
              {"final", "remote"}};
  AddBootstrapDns(dns["servers"]);
  if (const char* strategy = DnsStrategy(settings)) {
    dns["strategy"] = strategy;
  }
  Json config = Json::object();
  config["log"] = {{"level", "info"}, {"timestamp", true}};
  config["dns"] = std::move(dns);
  config["inbounds"] = Json::array({Json{{"type", "tun"},
                                         {"tag", "tun-in"},
                                         {"address", Json::array({"172.19.0.1/30", "fdfe:dcba:9876::1/126"})},
                                         {"auto_route", true},
                                         {"strict_route", true},
                                         {"stack", "mixed"}}});
  config["outbounds"] = Json::array({Json{{"type", "selector"}, {"tag", "proxy"}, {"outbounds", {"auto"}}, {"default", "auto"}},
                                     Json{{"type", "urltest"}, {"tag", "auto"}, {"outbounds", Json::array()}},
                                     Json{{"type", "direct"}, {"tag", "direct"}}});
  config["route"] = {{"rules", Json::array({Json{{"action", "sniff"}}, Json{{"protocol", "dns"}, {"action", "hijack-dns"}},
                                            Json{{"ip_is_private", true}, {"outbound", "direct"}}})},
                     {"final", "proxy"},
                     {"auto_detect_interface", true},
                     {"default_domain_resolver", kLocalDns}};
  return config.dump(2);
}

std::string ApplyRouting(std::string_view text, const RoutingSettings& settings,
                         const std::map<std::string, std::string>& files) {
  if (NestingDepth(text) > kMaxConfigDepth) {
    return std::string(text);
  }
  Json config = Json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!config.is_object()) {
    return std::string(text);
  }
  // Every top-level key first (an ordered_json object keeps its values in a
  // vector: adding one moves the others), references after.
  for (const char* key : {"outbounds", "dns", "route"}) {
    const bool list = std::string_view(key) == "outbounds";
    if (!config.contains(key) || (list ? !config[key].is_array() : !config[key].is_object())) {
      config[key] = list ? Json::array() : Json::object();
    }
  }
  if (WarpReady(settings) && (!config.contains("endpoints") || !config["endpoints"].is_array())) {
    config["endpoints"] = Json::array();
  }
  Json& outbounds = config["outbounds"];
  Json& dns = config["dns"];
  Json& route = config["route"];
  // Naive with "insecure": what a fork's export (Karing) writes - the official
  // core refuses the whole config for it ("insecure is not supported on naive
  // outbound"). Without it the server's certificate is checked, as Naive's is.
  for (Json& o : outbounds) {
    if (o.is_object() && Str(o, "type") == "naive" && o.contains("tls") && o["tls"].is_object()) {
      o["tls"].erase("insecure");
    }
  }
  // Hysteria 2 at the line's speed (RoutingSettings::hysteriaMbps): Brutal.
  if (settings.hysteriaMbps > 0) {
    for (Json& o : outbounds) {
      if (o.is_object() && Str(o, "type") == "hysteria2" && !o.contains("up_mbps") && !o.contains("down_mbps")) {
        o["up_mbps"] = settings.hysteriaMbps;
        o["down_mbps"] = settings.hysteriaMbps;
      }
    }
  }
  for (const char* key : {"rules", "rule_set"}) {
    if (!route.contains(key) || !route[key].is_array()) {
      route[key] = Json::array();
    }
  }
  for (const char* key : {"servers", "rules"}) {
    if (!dns.contains(key) || !dns[key].is_array()) {
      dns[key] = Json::array();
    }
  }

  // Where "directly" and "through the proxy" go in this config.
  std::string direct;
  for (const Json& o : outbounds) {
    if (Str(o, "type") == "direct") {
      direct = Str(o, "tag");
      break;
    }
  }
  if (direct.empty()) {
    const std::set<std::string> taken = Tags(outbounds);
    direct = "direct";
    for (int n = 2; taken.contains(direct); ++n) {
      direct = std::format("direct {}", n);
    }
    outbounds.push_back(Json{{"type", "direct"}, {"tag", direct}});
  }
  const std::string proxy = MainSelector(config);

  // WARP: its endpoint, over the proxy unless the user said directly.
  std::string warp;
  if (WarpReady(settings)) {
    std::set<std::string> taken = Tags(outbounds);
    taken.merge(Tags(config["endpoints"]));
    warp = std::string(kWarpTag);
    for (int n = 2; taken.contains(warp); ++n) {
      warp = std::format("{} {}", kWarpTag, n);
    }
    // Over the proxy: a selector of its own, not the proxy's. A switch of the
    // proxy's selector (the tray picks its server a second after every start)
    // moved WireGuard's UDP to another server, and Cloudflare took nothing from
    // the new address until WireGuard's next handshake - 15-25 s without WARP
    // (06.10.2026). Its server is the tray's to pick (main.cpp WatchWarp), by
    // WARP answering through it - not by the server's own speed: one can carry
    // TCP well and drop WireGuard's UDP (07.10.2026).
    std::string via;
    if (settings.warpViaProxy && !proxy.empty()) {
      std::vector<std::string> servers;
      GroupServers(outbounds, proxy, servers);
      if (!servers.empty()) {
        via = std::string(kWarpViaTag);
        for (int n = 2; taken.contains(via); ++n) {
          via = std::format("{} {}", kWarpViaTag, n);
        }
        outbounds.push_back(Json{{"type", "selector"}, {"tag", via}, {"outbounds", servers}});
      }
    }
    config["endpoints"].push_back(WarpEndpoint(*settings.warpAccount, warp, via));
  }

  // The lists that are there, as local files.
  std::set<std::string> lists;
  const std::set<std::string> haveSets = Tags(route["rule_set"]);
  for (const RuleSetSource& source : NeededRuleSets(settings)) {
    const auto file = files.find(source.tag);
    if (file == files.end()) {
      continue;
    }
    lists.insert(source.tag);
    if (!haveSets.contains(source.tag)) {
      route["rule_set"].push_back(Json{{"type", "local"},
                                       {"tag", source.tag},
                                       {"format", source.adguard ? "source" : "binary"},
                                       {"path", file->second}});
    }
  }
  if (route["rule_set"].empty()) {
    route.erase("rule_set");
  }
  // The ads lists there are - sing-geosite's, AdGuard's - and the cookie
  // windows' one: one rule for all.
  Json adsLists = Json::array();
  for (const std::string_view tag : {kAds, kAdsAdGuard, kCookieWindows}) {
    const bool on = tag == kCookieWindows ? settings.cookieWindows : settings.blockAds;
    if (on && lists.contains(std::string(tag))) {
      adsLists.push_back(std::string(tag));
    }
  }
  if (adsLists.size() == 1) {
    adsLists = Json(adsLists[0]);
  }
  const std::vector<std::string> russianNames = [&] {
    std::vector<std::string> names;
    for (const std::string_view tag : {kGeositeRu, kGeositeGovRu}) {
      if (lists.contains(std::string(tag))) {
        names.emplace_back(tag);
      }
    }
    return names;
  }();
  const std::vector<std::string> zones(kRussianZones.begin(), kRussianZones.end());

  // The local resolver for Russian and direct names.
  if (!Tags(dns["servers"]).contains(std::string(kLocalDns))) {
    dns["servers"].push_back(LocalDnsServer(settings.localDns, settings.localDnsCustom));
    AddBootstrapDns(dns["servers"]);
  }

  // Routing: the client's rules first, after sniffing and DNS hijacking.
  std::vector<Json> rules;
  // A rule's sites (names, keywords, addresses) and its programs match
  // apart, each part in its place below: in one sing-box rule they'd have to
  // match together - only that program, only to those sites - where the
  // user means either (before 0.4.78 "qwen.ai, Qwen.exe" missed the site in
  // a browser).
  const auto sites = [](const RouteRule& r) {
    Json m = Json::object();
    if (!r.domains.empty()) {
      m["domain_suffix"] = r.domains;
    }
    if (!r.keywords.empty()) {
      m["domain_keyword"] = r.keywords;
    }
    if (!r.ips.empty()) {
      m["ip_cidr"] = r.ips;
    }
    return m;
  };
  const auto programs = [](const RouteRule& r) {
    Json m = Json::object();
    if (!r.processes.empty()) {
      m["process_name"] = r.processes;
    }
    return m;
  };
  // The part `match` makes of the user's rules `pick` takes, in their order.
  const auto add = [&](const auto& pick, const auto& match) {
    for (const RouteRule& r : settings.rules) {
      if (!pick(r)) {
        continue;
      }
      Json rule = match(r);
      if (rule.empty()) {
        continue;  // nothing of that kind in it
      }
      if (r.action == RouteRule::Action::Block) {
        rule["action"] = "reject";
      } else if (r.action == RouteRule::Action::Proxy) {
        if (proxy.empty()) {
          continue;
        }
        rule["outbound"] = proxy;
      } else if (r.action == RouteRule::Action::Warp) {
        if (warp.empty()) {
          continue;  // WARP is off: the rule waits
        }
        rule["outbound"] = warp;
      } else {
        rule["outbound"] = direct;
      }
      rules.push_back(std::move(rule));
    }
  };
  const auto any = [](const RouteRule&) { return true; };
  // A blocked program first of all: no network, whatever it asks for.
  add([](const RouteRule& r) { return r.action == RouteRule::Action::Block; }, programs);
  // Then the sites, and the lists - ads, services, Russia, QUIC - before the
  // other programs: a site goes where it was sent from every program (the
  // bank directly even from a browser sent through WARP), and a browser sent
  // through the proxy has its ads cut too (before 0.4.58 it went past them).
  add(any, sites);
  // The ads (and the cookie windows) before the services: blocked is blocked
  // through any of them - YouTube's or Google's ad names, in their lists too,
  // went where the service went before 0.4.86. A site of the user's still wins.
  if (!adsLists.empty()) {
    rules.push_back(Json{{"rule_set", adsLists}, {"action", "reject"}});
  }
  // A service's app the user sent its own way (Telegram.exe while Telegram
  // goes elsewhere - "the app apart"): before the services, or the service's
  // names and addresses would take the app's traffic first.
  std::vector<std::string> serviceApps;  // of the services there are, lower case
  for (const ServiceRoute& s : settings.services) {
    if (const ServiceInfo* info = FindService(s.id)) {
      for (const std::string& app : ServiceApps(*info)) {
        serviceApps.push_back(Lower(app));
      }
    }
  }
  const auto appApart = [&](const RouteRule& r) {
    return std::any_of(r.processes.begin(), r.processes.end(), [&](const std::string& p) {
      return std::find(serviceApps.begin(), serviceApps.end(), Lower(p)) != serviceApps.end();
    });
  };
  add([&](const RouteRule& r) { return r.action != RouteRule::Action::Block && appApart(r); }, programs);
  // Services: each by its names and addresses, and its app - but one the
  // user has a rule of their own for - after the user's sites.
  std::set<std::string> ruled;  // programs with a rule of the user's, lower case
  for (const RouteRule& r : settings.rules) {
    for (const std::string& p : r.processes) {
      ruled.insert(Lower(p));
    }
  }
  std::vector<Json> games;  // the services' games: after the user's programs (below)
  for (const ServiceRoute& s : settings.services) {
    const ServiceInfo* info = FindService(s.id);
    if (info == nullptr) {
      continue;
    }
    Json sets = Json::array();
    for (const std::string& tag : {std::string(kServicePrefix) + s.id, ServiceIpsTag(s.id)}) {
      if (lists.contains(tag)) {
        sets.push_back(tag);
      }
    }
    std::vector<std::string> apps;
    for (const std::string& app : ServiceApps(*info)) {
      if (!ruled.contains(Lower(app))) {
        apps.push_back(app);
      }
    }
    if (sets.empty() && apps.empty() && info->games.empty()) {
      continue;
    }
    // Where it goes, as a rule's ending: an outbound or a refusal.
    Json to = Json::object();
    if (s.way == ServiceRoute::Way::Warp && !warp.empty()) {
      to["outbound"] = warp;
    } else if (s.way == ServiceRoute::Way::Direct) {
      to["outbound"] = direct;
    } else if (s.way == ServiceRoute::Way::Proxy && !proxy.empty()) {
      to["outbound"] = proxy;
    } else if (s.way == ServiceRoute::Way::Block) {
      to["action"] = "reject";
    } else if (s.way == ServiceRoute::Way::Country && !proxy.empty()) {
      // A selector of its own: the main group first (its default - as usual
      // until the tray knows which server is in that country), then the
      // servers; the tray switches it (box_select) as exits are measured.
      std::vector<std::string> members = {proxy};
      GroupServers(outbounds, proxy, members);
      const std::string out = ServiceOutboundTag(s.id);
      if (members.size() > 1 && !Tags(outbounds).contains(out)) {
        outbounds.push_back(Json{{"type", "selector"}, {"tag", out}, {"outbounds", members}, {"default", proxy}});
        to["outbound"] = out;
      }
    }
    if (to.empty()) {
      continue;
    }
    if (!sets.empty()) {
      Json rule = Json::object();
      rule["rule_set"] = sets.size() == 1 ? sets[0] : sets;
      rule.update(to);
      rules.push_back(std::move(rule));
    }
    if (!apps.empty()) {
      Json rule = Json::object();
      rule["process_name"] = apps;
      rule.update(to);
      rules.push_back(std::move(rule));
    }
    if (!info->games.empty()) {
      Json rule = Json::object();
      rule["process_path_regex"] = Json::array({std::string(info->games)});
      rule.update(to);
      games.push_back(std::move(rule));
    }
  }
  // Russia after the services: one sent its own way takes its Russian names
  // too (google.ru with Google) - the user's pick over the general list.
  if (settings.russiaDirect) {
    rules.push_back(Json{{"domain_suffix", zones}, {"outbound", direct}});
    if (!russianNames.empty()) {
      rules.push_back(Json{{"rule_set", russianNames}, {"outbound", direct}});
    }
    if (lists.contains(std::string(kGeoipRu))) {
      rules.push_back(Json{{"rule_set", kGeoipRu}, {"outbound", direct}});
    }
  }
  if (BlockQuic(settings, config)) {
    rules.push_back(Json{{"protocol", "quic"}, {"action", "reject"}});
  }
  add([&](const RouteRule& r) { return r.action != RouteRule::Action::Block && !appApart(r); }, programs);
  // The services' games after the user's programs: one sent its own way
  // (cs2.exe) goes there; the rest where their service goes - a game's own
  // traffic, to its servers' addresses, nothing before took.
  rules.insert(rules.end(), std::make_move_iterator(games.begin()), std::make_move_iterator(games.end()));
  std::size_t at = 0;
  for (const Json& r : route["rules"]) {
    const std::string action = Str(r, "action");
    if (action != "sniff" && action != "hijack-dns" && action != "resolve" && !r.contains("ip_is_private")) {
      break;
    }
    ++at;
  }
  Prepend(route["rules"], std::move(rules), at);
  route["final"] = settings.finalDirect || proxy.empty() ? direct : proxy;

  // DNS: Russian and direct names resolved locally, blocked ones not at all.
  std::vector<Json> dnsRules;
  // The servers' names directly, first of all: through the proxy the selected
  // AnyTLS-ECH's own ECH config fetch looped ("DNS query loopback") and its
  // connections failed for a minute after every start.
  if (const std::vector<std::string> servers = ServerNames(config); !servers.empty()) {
    dnsRules.push_back(Json{{"domain", servers}, {"server", kLocalDns}});
  }
  for (const RouteRule& r : settings.rules) {
    if (r.domains.empty() && r.keywords.empty()) {
      continue;
    }
    Json rule = Json::object();
    if (!r.domains.empty()) {
      rule["domain_suffix"] = r.domains;
    }
    if (!r.keywords.empty()) {
      rule["domain_keyword"] = r.keywords;
    }
    if (r.action == RouteRule::Action::Block) {
      SetNoAddress(rule);
    } else if (r.action == RouteRule::Action::Direct) {
      rule["server"] = kLocalDns;
    } else if (const std::string remote = Str(dns, "final"); !remote.empty()) {
      // Through the proxy or WARP: the config's own DNS - said here, ahead of
      // the ads lists' "no addresses", so an ad name sent there gets through.
      rule["server"] = remote;
    } else {
      continue;
    }
    dnsRules.push_back(std::move(rule));
  }
  // The ads before the services, as in the routing.
  if (!adsLists.empty()) {
    Json ads{{"rule_set", adsLists}};
    SetNoAddress(ads);
    dnsRules.push_back(std::move(ads));
  }
  // A service sent directly: its names here too (through the proxy's DNS
  // they'd get the proxy country's addresses); blocked: no addresses.
  for (const ServiceRoute& s : settings.services) {
    const std::string tag = std::string(kServicePrefix) + s.id;
    if (!lists.contains(tag)) {
      continue;
    }
    if (s.way == ServiceRoute::Way::Direct) {
      dnsRules.push_back(Json{{"rule_set", tag}, {"server", kLocalDns}});
    } else if (s.way == ServiceRoute::Way::Block) {
      Json blocked{{"rule_set", tag}};
      SetNoAddress(blocked);
      dnsRules.push_back(std::move(blocked));
    }
  }
  if (settings.russiaDirect) {
    dnsRules.push_back(Json{{"domain_suffix", zones}, {"server", kLocalDns}});
    if (!russianNames.empty()) {
      dnsRules.push_back(Json{{"rule_set", russianNames}, {"server", kLocalDns}});
    }
  }
  Prepend(dns["rules"], std::move(dnsRules), 0);
  AnswerTyposAtOnce(dns);
  if (dns["rules"].empty()) {
    dns.erase("rules");
  }
  if (const char* strategy = DnsStrategy(settings)) {
    dns["strategy"] = strategy;
  } else {
    dns.erase("strategy");
  }
  return config.dump(2, ' ', false, Json::error_handler_t::replace);
}

std::string DirectOutboundTag(std::string_view config) {
  const Json c =
      NestingDepth(config) > kMaxConfigDepth ? Json() : Json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!c.is_object() || !c.contains("outbounds") || !c["outbounds"].is_array()) {
    return {};
  }
  for (const Json& o : c["outbounds"]) {
    if (Str(o, "type") == "direct") {
      return Str(o, "tag");
    }
  }
  return {};
}

bool QuicBlocked(const RoutingSettings& settings, std::string_view config) {
  const Json c =
      NestingDepth(config) > kMaxConfigDepth ? Json() : Json::parse(config, nullptr, /*allow_exceptions=*/false);
  return c.is_object() ? BlockQuic(settings, c) : settings.quic != RoutingSettings::Quic::Allow;
}

std::vector<RoutingSettings::LocalDns> LocalDnsOrder(RoutingSettings::LocalDns pick) {
  using LocalDns = RoutingSettings::LocalDns;
  if (pick == LocalDns::Auto) {
    pick = LocalDns::Cloudflare;  // auto: the encrypted ones from Cloudflare, the system's last
  }
  std::vector<LocalDns> order{pick};
  for (const LocalDns dns : {LocalDns::Cloudflare, LocalDns::Google, LocalDns::Quad9, LocalDns::System}) {
    if (dns != pick && dns != LocalDns::System) {
      order.push_back(dns);
    }
  }
  if (pick != LocalDns::System) {
    order.push_back(LocalDns::System);
  }
  return order;
}

std::string_view LocalDnsAddress(RoutingSettings::LocalDns dns) {
  switch (dns) {
    case RoutingSettings::LocalDns::Auto:
    case RoutingSettings::LocalDns::Cloudflare: return "1.1.1.1";
    case RoutingSettings::LocalDns::Google: return "8.8.8.8";
    case RoutingSettings::LocalDns::Quad9: return "9.9.9.9";
    case RoutingSettings::LocalDns::System:
    case RoutingSettings::LocalDns::Custom: break;
  }
  return {};
}

std::string_view LocalDnsName(RoutingSettings::LocalDns dns) {
  switch (dns) {
    case RoutingSettings::LocalDns::Cloudflare: return "Cloudflare";
    case RoutingSettings::LocalDns::Google: return "Google";
    case RoutingSettings::LocalDns::Quad9: return "Quad9";
    case RoutingSettings::LocalDns::Auto: return "авто";
    case RoutingSettings::LocalDns::Custom: return "свой";
    case RoutingSettings::LocalDns::System: break;
  }
  return "системный";
}

bool LocalDnsUnreachable(std::string_view line, std::string_view address, int port) {
  if (address.empty()) {
    return false;
  }
  // As Go writes an address with its port: an IPv6 one in brackets.
  const std::string at = (address.find(':') != std::string_view::npos ? "[" + std::string(address) + "]"
                                                                        : std::string(address)) +
                         ":" + std::to_string(port);
  const std::size_t found = line.find(at);
  // The address itself, not the tail of a longer one ("11.1.1.1:443").
  if (found == std::string_view::npos || (found > 0 && (std::isdigit(static_cast<unsigned char>(line[found - 1])) != 0 ||
                                                        line[found - 1] == '.'))) {
    return false;
  }
  // Not sing-box's own "no route to internet": the box hasn't seen a network
  // yet (its first second after a start, every dial at once) - nothing about
  // the server; counted, it moved the DNS on at every start (08.10.2026).
  if (line.find("no route to internet") != std::string_view::npos) {
    return false;
  }
  return std::ranges::any_of(std::initializer_list<std::string_view>{"i/o timeout", "connection refused",
                                                                      "actively refused", "connection reset",
                                                                      "forcibly closed", "no route to host",
                                                                      "unreachable network", "deadline exceeded"},
                             [&](std::string_view why) { return line.find(why) != std::string_view::npos; });
}

std::optional<CustomDns> ParseCustomDns(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '/')) {
    text.remove_suffix(1);
  }
  if (text.empty() || text.size() > 300) {
    return std::nullopt;
  }
  CustomDns dns;
  dns.type = "udp";
  if (const std::size_t scheme = text.find("://"); scheme != std::string_view::npos) {
    static constexpr std::array<std::pair<std::string_view, std::pair<std::string_view, int>>, 5> kSchemes = {{
        {"udp", {"udp", 53}}, {"tcp", {"tcp", 53}}, {"tls", {"tls", 853}}, {"https", {"https", 443}},
        {"quic", {"quic", 853}},
    }};
    const std::string name = Lower(text.substr(0, scheme));
    const auto known = std::find_if(kSchemes.begin(), kSchemes.end(), [&](const auto& s) { return s.first == name; });
    if (known == kSchemes.end()) {
      return std::nullopt;
    }
    dns.type = std::string(known->second.first);
    dns.port = known->second.second;
    text.remove_prefix(scheme + 3);
  }
  // A path: DoH's only.
  if (const std::size_t slash = text.find('/'); slash != std::string_view::npos) {
    if (dns.type != "https") {
      return std::nullopt;
    }
    dns.path = std::string(text.substr(slash));
    text = text.substr(0, slash);
  } else if (dns.type == "https") {
    dns.path = "/dns-query";
  }
  if (dns.path.find_first_of(" \t\"\\") != std::string::npos) {
    return std::nullopt;
  }
  // The host, and a port after it: "[v6]:port", "v4:port", "name:port".
  std::string_view port;
  if (text.starts_with('[')) {
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    dns.host = std::string(text.substr(1, close - 1));
    const std::string_view rest = text.substr(close + 1);
    if (!rest.empty() && !rest.starts_with(':')) {
      return std::nullopt;
    }
    port = rest.empty() ? rest : rest.substr(1);
    if (dns.host.find(':') == std::string::npos ||
        !std::all_of(dns.host.begin(), dns.host.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0 || c == ':' || c == '.'; })) {
      return std::nullopt;
    }
  } else {
    const std::size_t colon = text.rfind(':');
    if (colon != std::string_view::npos && text.find(':') != colon) {
      return std::nullopt;  // a bare IPv6 address: in brackets, to tell its port apart
    }
    dns.host = Lower(text.substr(0, colon == std::string_view::npos ? text.size() : colon));
    port = colon == std::string_view::npos ? std::string_view() : text.substr(colon + 1);
    const bool dotted = std::all_of(dns.host.begin(), dns.host.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.'; });
    if (dotted) {
      int parts = 0;
      for (std::string_view rest = dns.host; ; ++parts) {
        const std::size_t dot = std::min(rest.find('.'), rest.size());
        const std::string_view part = rest.substr(0, dot);
        if (part.empty() || part.size() > 3 || std::stoi(std::string(part)) > 255) {
          return std::nullopt;
        }
        if (dot == rest.size()) {
          break;
        }
        rest.remove_prefix(dot + 1);
      }
      if (parts != 3) {
        return std::nullopt;
      }
    } else {
      // A name: letters, digits, hyphens, at least one dot, not at its ends.
      dns.named = true;
      if (dns.host.size() > 253 || dns.host.find('.') == std::string::npos || dns.host.front() == '.' ||
          dns.host.back() == '.' || dns.host.front() == '-' ||
          !std::all_of(dns.host.begin(), dns.host.end(), [](char c) { return IsDomainChar(c); })) {
        return std::nullopt;
      }
    }
  }
  if (!port.empty()) {
    if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
      return std::nullopt;
    }
    dns.port = std::stoi(std::string(port));
    if (dns.port < 1 || dns.port > 65535) {
      return std::nullopt;
    }
  }
  return dns;
}

bool HasHysteria2(std::string_view config) {
  const Json c = Json::parse(config, nullptr, false);
  if (!c.is_object() || !c.contains("outbounds") || !c["outbounds"].is_array()) {
    return false;
  }
  return std::any_of(c["outbounds"].begin(), c["outbounds"].end(),
                     [](const Json& o) { return o.is_object() && Str(o, "type") == "hysteria2"; });
}

std::vector<std::string> WarpViaServers(std::string_view config) {
  const Json c = Json::parse(config, nullptr, false);
  if (!c.is_object() || !c.contains("outbounds") || !c["outbounds"].is_array()) {
    return {};
  }
  for (const Json& o : c["outbounds"]) {
    const auto tag = o.is_object() ? o.find("tag") : o.end();
    if (tag != o.end() && tag->is_string() && tag->get<std::string>() == kWarpViaTag && o.contains("outbounds") &&
        o["outbounds"].is_array()) {
      std::vector<std::string> servers;
      for (const Json& server : o["outbounds"]) {
        if (server.is_string()) {
          servers.push_back(server.get<std::string>());
        }
      }
      return servers;
    }
  }
  return {};
}

bool WarpStalled(std::string_view line) {
  // "wireguard[warp]: connect tcp 140.82.121.3:443: operation timed out",
  // "... - failed to send handshake initiation: no known endpoint for peer",
  // "... connect to server: failed to create session: dial tcp ..." (07.10.2026).
  if (line.find(std::format("wireguard[{}]", kWarpTag)) == std::string_view::npos) {
    return false;
  }
  return std::ranges::any_of(std::initializer_list<std::string_view>{"timed out", "i/o timeout", "deadline exceeded",
                                                                      "failed to send handshake",
                                                                      "failed to create session"},
                             [&](std::string_view why) { return line.find(why) != std::string_view::npos; });
}

}  // namespace sovereign::tray
