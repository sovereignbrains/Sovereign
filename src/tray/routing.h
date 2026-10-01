#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Where traffic goes and how names are resolved - the client's to decide,
// not each subscription's. The rules here come first in whatever config runs
// (sing-box takes the first rule that matches), and in "own" mode the tray
// builds the whole frame - TUN, DNS, routing - with the subscriptions giving
// only servers (combine.h). In "profile" mode one configuration's own routing
// and DNS stay, behind these rules.
//
// Russia directly is on by default: a Russian site or app that sees the proxy
// server's address can tie it to the user (and the server to a VPN), so
// Russian domains, Russian IPs and their DNS go around the proxy.
//
// No Win32 here: unit-tested, the configs through the pinned sing-box's
// `check`, in tests/unit/routing_test.cpp.

namespace sovereign::tray {

struct RouteRule {
  enum class Action : std::uint8_t { Direct, Proxy, Block };
  std::vector<std::string> domains;    // domain_suffix: the domain and its subdomains
  std::vector<std::string> keywords;   // domain_keyword
  std::vector<std::string> ips;        // ip_cidr ("1.2.3.0/24", a bare address is /32 or /128)
  std::vector<std::string> processes;  // process_name ("app.exe")
  Action action = Action::Direct;
};

struct RoutingSettings {
  enum class Source : std::uint8_t { Own, Profile };
  enum class RemoteDns : std::uint8_t { Cloudflare, Google, Quad9 };
  // Direct names - Russian, the user's direct ones, the servers' own - over
  // encrypted DNS straight from here (the provider doesn't see what's
  // asked) while it works; the system's resolver when it doesn't.
  enum class LocalDns : std::uint8_t { Cloudflare, Google, System };
  Source source = Source::Own;
  std::string sourceProfile;  // Profile: the configuration whose routing and DNS stay (its id)
  bool russiaDirect = true;
  bool blockAds = true;
  bool blockQuic = true;
  bool finalDirect = false;  // what no rule matches: directly (else through the proxy)
  RemoteDns remoteDns = RemoteDns::Cloudflare;  // through the proxy, over HTTPS
  LocalDns localDns = LocalDns::Cloudflare;     // directly: Russian and direct names, the servers' own
  bool ipv4Only = true;
  std::vector<RouteRule> rules;  // the user's, first of all
};

// tray.json's "routing", read defensively (a wrong field keeps its default).
nlohmann::json RoutingToJson(const RoutingSettings& settings);
RoutingSettings RoutingFromJson(const nlohmann::json& json);

// What the user typed for a rule: "qwen.ai, alicdn.com 10.0.0.0/8 app.exe" -
// names, address ranges and programs told apart. Nothing usable: nullopt.
std::optional<RouteRule> ParseRule(std::string_view text, RouteRule::Action action);
// A rule as one line: "qwen.ai, alicdn.com, 10.0.0.0/8".
std::string RuleText(const RouteRule& rule);

// A config's own simple rules - names, keywords, address ranges, programs
// sent directly, through its proxy or blocked - as the user's: what
// "copy from the subscription" takes, and the first start of this keeps.
std::vector<RouteRule> ImportRules(std::string_view config);
// `into` with `more` added, the ones it has already left out.
void MergeRules(std::vector<RouteRule>& into, const std::vector<RouteRule>& more);

// The lists the rules need: downloaded by the tray (rule_set.srs files),
// given to the box as local files - a list that can't be fetched never
// keeps the box from starting.
struct RuleSetSource {
  std::string tag;   // in the config, and the file's name: <tag>.srs
  std::string url;
  bool ips = false;  // addresses (geoip): routing only, not DNS
};
std::vector<RuleSetSource> NeededRuleSets(const RoutingSettings& settings);
// What a download must start with to be a list (sing-box's "SRS" magic) -
// not a captive portal's page or an error text.
inline bool IsRuleSetFile(std::string_view bytes) { return bytes.starts_with("SRS"); }

// The frame of "own" mode: TUN, DNS (remote through "proxy", local direct),
// sniffing and DNS hijacking, the local network directly, a "proxy"
// selector with an "auto" URL test for the configurations' servers.
std::string OwnFrame(const RoutingSettings& settings);

// `config` with the client's rules first: the user's, ads blocked, Russia
// directly (names and addresses; their DNS locally), QUIC blocked - and, in
// own mode, what no rule matches. `files`: the lists there are, tag -> path;
// a missing one is left out (the domain zones still go directly). JSON text,
// or `config` unchanged if it isn't a JSON object.
std::string ApplyRouting(std::string_view config, const RoutingSettings& settings,
                         const std::map<std::string, std::string>& files);

}  // namespace sovereign::tray
