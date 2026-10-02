#include "routing.h"

#include "config_sync.h"

#include <algorithm>
#include <array>
#include <format>
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
constexpr const char* kLocalDns = "sov-local";

constexpr std::string_view kGeositeBase = "https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set/";
constexpr std::string_view kGeoipBase = "https://raw.githubusercontent.com/SagerNet/sing-geoip/rule-set/";

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

// The own frame's DNS strategy: IPv4 only, unless IPv6 goes through WARP -
// then both, IPv4 first; nothing when IPv6 is simply allowed.
const char* DnsStrategy(const RoutingSettings& s) {
  if (WarpReady(s) && s.warpIpv6) {
    return "prefer_ipv4";
  }
  return s.ipv4Only ? "ipv4_only" : nullptr;
}

const char* ActionName(RouteRule::Action action) {
  switch (action) {
    case RouteRule::Action::Direct: return "direct";
    case RouteRule::Action::Proxy: return "proxy";
    case RouteRule::Action::Block: return "block";
    case RouteRule::Action::Warp: return "warp";
  }
  return "direct";
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

Json LocalDnsServer(RoutingSettings::LocalDns local) {
  switch (local) {
    case RoutingSettings::LocalDns::Cloudflare: return Json{{"type", "https"}, {"tag", kLocalDns}, {"server", "1.1.1.1"}};
    case RoutingSettings::LocalDns::Google: return Json{{"type", "https"}, {"tag", kLocalDns}, {"server", "8.8.8.8"}};
    case RoutingSettings::LocalDns::System: break;
  }
  return Json{{"type", "local"}, {"tag", kLocalDns}};
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

}  // namespace

nlohmann::json RoutingToJson(const RoutingSettings& s) {
  nlohmann::json rules = nlohmann::json::array();
  for (const RouteRule& r : s.rules) {
    rules.push_back({{"domains", r.domains},
                     {"keywords", r.keywords},
                     {"ips", r.ips},
                     {"processes", r.processes},
                     {"action", ActionName(r.action)}});
  }
  return {{"source", s.source == RoutingSettings::Source::Own ? "own" : "profile"},
          {"sourceProfile", s.sourceProfile},
          {"russiaDirect", s.russiaDirect},
          {"blockAds", s.blockAds},
          {"blockQuic", s.blockQuic},
          {"finalDirect", s.finalDirect},
          {"remoteDns", s.remoteDns == RoutingSettings::RemoteDns::Google  ? "google"
                        : s.remoteDns == RoutingSettings::RemoteDns::Quad9 ? "quad9"
                                                                            : "cloudflare"},
          {"localDns", s.localDns == RoutingSettings::LocalDns::Google   ? "google"
                       : s.localDns == RoutingSettings::LocalDns::System ? "system"
                                                                          : "cloudflare"},
          {"ipv4Only", s.ipv4Only},
          {"rules", std::move(rules)},
          {"warp", s.warp},
          {"warpViaProxy", s.warpViaProxy},
          {"warpIpv6", s.warpIpv6},
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
  s.source = text("source") == "profile" ? RoutingSettings::Source::Profile : RoutingSettings::Source::Own;
  s.sourceProfile = text("sourceProfile");
  flag("russiaDirect", s.russiaDirect);
  flag("blockAds", s.blockAds);
  flag("blockQuic", s.blockQuic);
  flag("finalDirect", s.finalDirect);
  flag("ipv4Only", s.ipv4Only);
  flag("warp", s.warp);
  flag("warpViaProxy", s.warpViaProxy);
  flag("warpIpv6", s.warpIpv6);
  if (const auto it = json.find("warpAccount"); it != json.end()) {
    s.warpAccount = WarpFromJson(*it);
  }
  const std::string remote = text("remoteDns");
  s.remoteDns = remote == "google"  ? RoutingSettings::RemoteDns::Google
                : remote == "quad9" ? RoutingSettings::RemoteDns::Quad9
                                    : RoutingSettings::RemoteDns::Cloudflare;
  const std::string local = text("localDns");
  s.localDns = local == "google"   ? RoutingSettings::LocalDns::Google
               : local == "system" ? RoutingSettings::LocalDns::System
                                   : RoutingSettings::LocalDns::Cloudflare;
  if (const auto it = json.find("rules"); it != json.end() && it->is_array()) {
    for (const auto& entry : *it) {
      if (!entry.is_object()) {
        continue;
      }
      std::string line;
      for (const char* key : {"domains", "keywords", "ips", "processes"}) {
        if (const auto values = entry.find(key); values != entry.end()) {
          for (const auto& v : *values) {
            if (v.is_string()) {
              // Keywords keep their kind through the text: "~word".
              line += (std::string_view(key) == "keywords" ? "~" : "") + v.get<std::string>() + " ";
            }
          }
        }
      }
      const std::string action = entry.contains("action") && entry["action"].is_string() ? entry["action"].get<std::string>() : "";
      const auto rule = ParseRule(line, action == "block"   ? RouteRule::Action::Block
                                        : action == "proxy" ? RouteRule::Action::Proxy
                                        : action == "warp"  ? RouteRule::Action::Warp
                                                            : RouteRule::Action::Direct);
      if (rule) {
        s.rules.push_back(*rule);
      }
    }
  }
  return s;
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
    if (Lower(t).ends_with(".exe") && t.size() > 4 && t.find_first_of("\\/:*?\"<>|") == std::string::npos) {
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
  for (const char c : text) {
    if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      take();
    } else if (token.size() < 300) {
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
    if (!simple) {
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

std::vector<RuleSetSource> NeededRuleSets(const RoutingSettings& settings) {
  std::vector<RuleSetSource> sets;
  if (settings.russiaDirect) {
    sets.push_back({std::string(kGeositeRu), std::string(kGeositeBase) + "geosite-category-ru.srs", false});
    sets.push_back({std::string(kGeositeGovRu), std::string(kGeositeBase) + "geosite-category-gov-ru.srs", false});
    sets.push_back({std::string(kGeoipRu), std::string(kGeoipBase) + "geoip-ru.srs", true});
  }
  if (settings.blockAds) {
    sets.push_back({std::string(kAds), std::string(kGeositeBase) + "geosite-category-ads-all.srs", false});
  }
  return sets;
}

std::string OwnFrame(const RoutingSettings& settings) {
  const char* remote = settings.remoteDns == RoutingSettings::RemoteDns::Google  ? "8.8.8.8"
                       : settings.remoteDns == RoutingSettings::RemoteDns::Quad9 ? "9.9.9.9"
                                                                                  : "1.1.1.1";
  Json dns = {{"servers", Json::array({Json{{"type", "https"}, {"tag", "remote"}, {"server", remote}, {"detour", "proxy"}},
                                       LocalDnsServer(settings.localDns)})},
              {"final", "remote"}};
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
    config["endpoints"].push_back(
        WarpEndpoint(*settings.warpAccount, warp, settings.warpViaProxy ? proxy : std::string()));
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
      route["rule_set"].push_back(
          Json{{"type", "local"}, {"tag", source.tag}, {"format", "binary"}, {"path", file->second}});
    }
  }
  if (route["rule_set"].empty()) {
    route.erase("rule_set");
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
    dns["servers"].push_back(LocalDnsServer(settings.localDns));
  }

  // Routing: the client's rules first, after sniffing and DNS hijacking.
  std::vector<Json> rules;
  const auto match = [](const RouteRule& r) {
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
    if (!r.processes.empty()) {
      m["process_name"] = r.processes;
    }
    return m;
  };
  for (const RouteRule& r : settings.rules) {
    Json rule = match(r);
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
  if (settings.blockAds && lists.contains(std::string(kAds))) {
    rules.push_back(Json{{"rule_set", kAds}, {"action", "reject"}});
  }
  if (!warp.empty() && settings.warpIpv6) {
    rules.push_back(Json{{"ip_version", 6}, {"outbound", warp}});
  }
  if (settings.russiaDirect) {
    rules.push_back(Json{{"domain_suffix", zones}, {"outbound", direct}});
    if (!russianNames.empty()) {
      rules.push_back(Json{{"rule_set", russianNames}, {"outbound", direct}});
    }
    if (lists.contains(std::string(kGeoipRu))) {
      rules.push_back(Json{{"rule_set", kGeoipRu}, {"outbound", direct}});
    }
  }
  if (settings.blockQuic) {
    rules.push_back(Json{{"protocol", "quic"}, {"action", "reject"}});
  }
  std::size_t at = 0;
  for (const Json& r : route["rules"]) {
    const std::string action = Str(r, "action");
    if (action != "sniff" && action != "hijack-dns" && action != "resolve" && !r.contains("ip_is_private")) {
      break;
    }
    ++at;
  }
  Prepend(route["rules"], std::move(rules), at);
  if (settings.source == RoutingSettings::Source::Own) {
    route["final"] = settings.finalDirect || proxy.empty() ? direct : proxy;
  }

  // DNS: Russian and direct names resolved locally, blocked ones not at all.
  std::vector<Json> dnsRules;
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
      rule["action"] = "reject";
    } else if (r.action == RouteRule::Action::Direct) {
      rule["server"] = kLocalDns;
    } else {
      continue;  // through the proxy: the config's own DNS
    }
    dnsRules.push_back(std::move(rule));
  }
  if (settings.blockAds && lists.contains(std::string(kAds))) {
    dnsRules.push_back(Json{{"rule_set", kAds}, {"action", "reject"}});
  }
  if (settings.russiaDirect) {
    dnsRules.push_back(Json{{"domain_suffix", zones}, {"server", kLocalDns}});
    if (!russianNames.empty()) {
      dnsRules.push_back(Json{{"rule_set", russianNames}, {"server", kLocalDns}});
    }
  }
  Prepend(dns["rules"], std::move(dnsRules), 0);
  if (dns["rules"].empty()) {
    dns.erase("rules");
  }
  if (settings.source == RoutingSettings::Source::Own) {
    if (const char* strategy = DnsStrategy(settings)) {
      dns["strategy"] = strategy;
    } else {
      dns.erase("strategy");
    }
  }
  return config.dump(2, ' ', false, Json::error_handler_t::replace);
}

}  // namespace sovereign::tray
