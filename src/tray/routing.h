#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "warp.h"

// Where traffic goes and how names are resolved - the client's to decide,
// not each subscription's. The tray builds the whole frame - TUN, DNS,
// routing - and the subscriptions give only servers (combine.h); their own
// routing and DNS never get in (dropped in 0.4.29: one client's routing for
// everyone - services like Netflix are the client's to send, through WARP
// or a country).
//
// Russia directly is the default: a Russian site or app that sees the proxy
// server's address can tie it to the user (and the server to a VPN), so
// Russian domains, Russian IPs and their DNS go around the proxy. Through
// WARP they see Cloudflare's address instead, neither the user's nor the
// server's; or they can be blocked (RoutingSettings::russia).
//
// No Win32 here: unit-tested, the configs through the pinned sing-box's
// `check`, in tests/unit/routing_test.cpp.

namespace sovereign::tray {

struct RouteRule {
  enum class Action : std::uint8_t { Direct, Proxy, Block, Warp };  // Warp: the WARP endpoint (warp.h), if it's on
  std::vector<std::string> domains;    // domain_suffix: the domain and its subdomains
  std::vector<std::string> keywords;   // domain_keyword
  std::vector<std::string> ips;        // ip_cidr ("1.2.3.0/24", a bare address is /32 or /128)
  std::vector<std::string> processes;  // process_name ("app.exe")
  Action action = Action::Direct;
  // What the user calls it - "Qwen" for its sites and its app together, a
  // service of their own; empty: none, the board says what's in it.
  // Sites and programs of a rule match either, not both (ApplyRouting).
  std::string name;
};

// A whole service sent its own way: Netflix through WARP (from a VPS's
// country it cuts its catalogue or doesn't let in at all). Matched by its
// SagerNet sing-geosite list - nothing for the user to type.
struct ServiceRoute {
  // Warp: through WARP. Country: through the fastest server whose measured
  // exit is in `country`, whatever the provider named it - picked by the
  // tray as the exits and delays come in (PickCountryServer), switched with
  // box_select, no restart; none there: as usual. Direct: around the proxy,
  // its names resolved here too. Proxy: through the proxy whatever else says
  // otherwise (Russian sites directly, everything else directly). Block: not
  // at all.
  enum class Way : std::uint8_t { Warp, Country, Direct, Proxy, Block };
  std::string id;       // ServiceCatalog's, "netflix"
  Way way = Way::Warp;
  std::string country;  // Country: "US"
};

// The outbound a service sent through a country goes to: a selector of the
// main group (its default: as usual) and the servers.
std::string ServiceOutboundTag(std::string_view id);

// The server for a service through `country`: of `servers` (the main group's,
// in order), the ones whose exit is measured in `country`; the fastest by
// delay, one not measured yet after any that is. Empty if none is there.
std::string PickCountryServer(std::string_view country, const std::vector<std::string>& servers,
                              const std::map<std::string, std::string>& exitCountry,
                              const std::map<std::string, int>& delayMs);

// "Нидерланды" for "NL" (the countries VPN servers are usually in); the code
// itself for others.
std::string CountryName(std::string_view code);
// The same after "через": "Германию" for "DE", "Польшу" for "PL"; "США",
// "Нидерланды" as they are.
std::string CountryNameVia(std::string_view code);

// Two uppercase Latin letters.
bool IsCountryCode(std::string_view code);
// A service is its site and its app together: its names (the geosite
// list), its addresses where its app dials them rather than names
// (Telegram's), and its desktop app's exes - picked once, it takes all three.
struct ServiceInfo {
  std::string_view id;
  std::string_view name;     // as people know it: "Netflix"
  std::string_view geosite;  // the list: geosite-<geosite>.srs
  std::string_view apps;     // its desktop app's exe names, ","-separated: "steam.exe,steamwebhelper.exe"
  std::string_view geoip;    // a list of its addresses (an .srs URL); empty: none
  // Where the games it installs are (a process_path_regex): a game is a
  // program of its own, dialing its servers' addresses - neither the
  // service's names nor its app catch it. Empty: none.
  std::string_view games;
};
// A service's apps, one name each.
std::vector<std::string> ServiceApps(const ServiceInfo& service);
// The services that can be sent their own way, in the order offered.
std::span<const ServiceInfo> ServiceCatalog();
const ServiceInfo* FindService(std::string_view id);
// The service a program is the app of, by its exe's name: one of its apps
// ("EpicGamesLauncher.exe"), or named as it ("Telegram.exe"); nullptr: none.
const ServiceInfo* ServiceOfProgram(std::string_view exe);

struct RoutingSettings {
  // Auto: Cloudflare (through the VPN which one hardly matters: the provider
  // sees none of it). Custom: remoteDnsCustom, as typed (ParseCustomDns).
  enum class RemoteDns : std::uint8_t { Cloudflare, Google, Quad9, Auto, Custom };
  // Direct names - Russian, the user's direct ones, the servers' own - over
  // encrypted DNS straight from here (the provider doesn't see what's
  // asked). Its one server is the user's pick; when that stops answering
  // (an ISP blocks its address) the tray moves the box on to the next of
  // LocalDnsOrder, the system's resolver last - the pick stays as it is.
  // Auto: that order from Cloudflare - the default. Custom: localDnsCustom,
  // first (ParseCustomDns); given way from only when dialled by its address.
  enum class LocalDns : std::uint8_t { Cloudflare, Google, System, Quad9, Auto, Custom };
  // Where the Russian sites go - Russia's domain zones, its names' lists and
  // its addresses: directly (the default, their names resolved here too),
  // through the proxy, blocked (no addresses for their names either) or
  // through WARP (waits while WARP is off, as a rule does). After the
  // user's sites and the services, before the programs. Nullopt: no way of
  // their own - where everything else goes.
  std::optional<RouteRule::Action> russia = RouteRule::Action::Direct;
  bool blockAds = true;
  // The "accept cookies?" windows of sites: the names their scripts are
  // loaded from (OneTrust, Cookiebot, Usercentrics...) given no addresses, so
  // the window never comes up and nothing is agreed to. Only a window that's
  // someone else's script - a site's own stays; and a site that waits for the
  // answer may not work without it, so it's off until asked for.
  bool cookieWindows = false;
  // QUIC (HTTP/3 over UDP) refused, so browsers take TCP. Auto: refused
  // unless every server carries UDP as UDP (Hysteria, TUIC) - through a TCP
  // one (VLESS, Trojan, AnyTLS...) it's UDP packed in TCP, worse than TCP.
  enum class Quic : std::uint8_t { Auto, Block, Allow };
  Quic quic = Quic::Auto;
  // The line's speed for Hysteria 2, Mbit/s each way: told to the server
  // (up_mbps/down_mbps), it sends at that rate whatever the losses (Brutal) -
  // steady over a lossy Wi-Fi, but more than the line takes floods it. 0: the
  // server picks its rate (BBR). A config's own speeds stay.
  int hysteriaMbps = 0;
  bool finalDirect = false;  // what no rule matches: directly (else through the proxy)
  RemoteDns remoteDns = RemoteDns::Auto;  // through the proxy
  LocalDns localDns = LocalDns::Auto;     // directly: Russian and direct names, the servers' own
  std::string remoteDnsCustom;            // RemoteDns::Custom's address, as typed
  std::string localDnsCustom;             // LocalDns::Custom's
  bool ipv4Only = true;
  std::vector<RouteRule> rules;  // the user's, first of all
  std::vector<ServiceRoute> services;  // after the user's rules

  // Cloudflare WARP (warp.h): an endpoint the box has while warp is on and
  // there's an account; only rules with Action::Warp go to it (IPv6 of
  // everything through it made WARP the way out for most sites - dropped).
  bool warp = false;
  bool warpViaProxy = true;  // WireGuard to Cloudflare over the proxy (else directly)
  std::optional<WarpAccount> warpAccount;
};

// The WARP endpoint's tag in the config.
inline constexpr std::string_view kWarpTag = "warp";
// Over the proxy, WARP goes out through this selector of the proxy's servers;
// which one is the tray's to pick (main.cpp WatchWarp).
inline constexpr std::string_view kWarpViaTag = "warp-via";

// Whether the box gets the WARP endpoint: on, registered, and some rule
// sends sites to it - an endpoint nobody uses only keeps a tunnel up for
// nothing (and over the proxy it broke every 5 min idle, filling the log).
inline bool WarpReady(const RoutingSettings& s) {
  return s.warp && s.warpAccount.has_value() &&
         (s.russia == RouteRule::Action::Warp ||
          std::any_of(s.rules.begin(), s.rules.end(), [](const RouteRule& r) { return r.action == RouteRule::Action::Warp; }) ||
          std::any_of(s.services.begin(), s.services.end(),
                      [](const ServiceRoute& r) { return r.way == ServiceRoute::Way::Warp; }));
}

// tray.json's "routing", read defensively (a wrong field keeps its default).
nlohmann::json RoutingToJson(const RoutingSettings& settings);
RoutingSettings RoutingFromJson(const nlohmann::json& json);

// What the user typed for a rule: "qwen.ai, alicdn.com 10.0.0.0/8 app.exe" -
// names, address ranges and programs told apart; a program with a space in
// its name goes in quotes ("SSH Client.exe"). Nothing usable: nullopt.
std::optional<RouteRule> ParseRule(std::string_view text, RouteRule::Action action);
// A rule for one program by its exe's name as it is - "SSH Client.exe" with
// its space, which typed text would split (there it takes quotes). Nullopt
// if it isn't an exe's name.
std::optional<RouteRule> ProgramRule(std::string_view exe, RouteRule::Action action);
// A rule as one line: "qwen.ai, alicdn.com, 10.0.0.0/8".
std::string RuleText(const RouteRule& rule);

// A config's own simple rules - names, keywords, address ranges, programs
// sent directly, through its proxy or blocked - as the user's: what
// "copy from the subscription" takes, and the first start of this keeps.
std::vector<RouteRule> ImportRules(std::string_view config);
// `into` with `more` added, the ones it has already left out.
void MergeRules(std::vector<RouteRule>& into, const std::vector<RouteRule>& more);
// `rule` put on the board: a rule for the same things in another bucket
// gives way - one thing, one bucket (only the first of them ever worked).
void AddRule(std::vector<RouteRule>& rules, const RouteRule& rule);

// A rule as it's typed back to ParseRule: RuleText, a program with a space
// in quotes - what editing it starts from.
std::string RuleInput(const RouteRule& rule);
// The rule at `index` holding what `text` says instead (ParseRule's), its
// way and name kept; another rule for the same things gives way. False:
// nothing understood (or no such rule), nothing changed.
bool EditRule(std::vector<RouteRule>& rules, std::size_t index, std::string_view text);
// `exe` added to the rule at `index` (not twice, case-blind): the app with
// its sites, either matching. A rule that was that program alone goes - it's
// part of this one now. False: not an exe's name, or no such rule.
bool AddProgramToRule(std::vector<RouteRule>& rules, std::size_t index, std::string_view exe);
// A rule's name as kept: one line, spaces trimmed, at most 40 characters.
std::string CleanRuleName(std::string_view name);

// The lists the rules need: downloaded by the tray (rule_set.srs files),
// given to the box as local files - a list that can't be fetched never
// keeps the box from starting.
struct RuleSetSource {
  std::string tag;   // in the config, and the file's name: <tag>.srs (AdGuard's: .json)
  std::string url;
  bool ips = false;  // addresses (geoip): routing only, not DNS
  bool adguard = false;  // an AdGuard filter, text: AdGuardRuleSet makes it a source rule-set
};
std::vector<RuleSetSource> NeededRuleSets(const RoutingSettings& settings);
// A check of the lists as it goes (the tray's UpdateRuleSets): one download
// a poll, each list tried once a round - one that fails isn't fetched again
// every second, nor does it hold back the ones after it - and the box gets
// the lists only when the round is over: one restart for them all, not one a
// list.
class RuleSetRound {
 public:
  // Of the lists `due` (missing or a day old, in NeededRuleSets' order), the
  // first one not tried in this round, taken as tried now; nullopt: all tried.
  std::optional<std::string> Next(std::span<const std::string> due);
  // Every list `due` tried: the round is over.
  bool Over(std::span<const std::string> due) const;

 private:
  std::set<std::string> tried_;
};
// What a download must start with to be a list (sing-box's "SRS" magic) -
// not a captive portal's page or an error text.
inline bool IsRuleSetFile(std::string_view bytes) { return bytes.starts_with("SRS"); }
// An AdGuard filter ("||ads.example.com^", "@@||ok.example.com^" its
// exceptions) as a sing-box source rule-set, JSON: the plain names, the rest
// (masks, regular expressions, ~0.4%) left out. A browser filter's
// "||cmp.example.com^$third-party" is a plain name too: by name there's no
// telling whose page asks, so it's blocked everywhere; its rules for a path
// or for some sites only are left out. Nullopt if it has none - not a filter.
std::optional<std::string> AdGuardRuleSet(std::string_view filter);

// The frame of "own" mode: TUN, DNS (remote through "proxy", local direct),
// sniffing and DNS hijacking, the local network directly, a "proxy"
// selector with an "auto" URL test for the configurations' servers.
std::string OwnFrame(const RoutingSettings& settings);

// `config` with the client's rules first: the user's, ads blocked, Russia its
// way (names and addresses; directly, their DNS locally), QUIC blocked - and, in
// own mode, what no rule matches. `files`: the lists there are, tag -> path;
// a missing one is left out (the domain zones still go directly). JSON text,
// or `config` unchanged if it isn't a JSON object.
std::string ApplyRouting(std::string_view config, const RoutingSettings& settings,
                         const std::map<std::string, std::string>& files);

// The tag of `config`'s first direct outbound - the way around the proxy the
// tray measures the user's own network by - or empty.
std::string DirectOutboundTag(std::string_view config);

// Whether `config` with these settings refuses QUIC - what Quic::Auto comes
// to with its servers.
bool QuicBlocked(const RoutingSettings& settings, std::string_view config);

// The local DNS servers in the order a failing one gives way: the user's
// pick, then the other encrypted ones (Cloudflare, Google, Quad9), the
// system's resolver last.
std::vector<RoutingSettings::LocalDns> LocalDnsOrder(RoutingSettings::LocalDns pick);

// The address an encrypted local server is dialled at ("1.1.1.1"; Auto:
// Cloudflare's); empty for the system's resolver and a custom one.
std::string_view LocalDnsAddress(RoutingSettings::LocalDns dns);

// "Cloudflare", "Google", "Quad9", "системный", "авто", "свой".
std::string_view LocalDnsName(RoutingSettings::LocalDns dns);

// Whether a core log line says the local server at `address`:`port` couldn't
// be reached: a dial to it timed out, was refused or reset, had no route. A
// query that timed out without saying where isn't counted - it may have been
// the remote DNS, through the proxy.
bool LocalDnsUnreachable(std::string_view line, std::string_view address, int port = 443);

// A DNS server the user typed: "8.8.8.8", "1.2.3.4:5353", "tls://dns.example",
// "https://dns.example/dns-query", "quic://dns.example", "tcp://..." -
// UDP when no scheme. Nullopt: not one.
struct CustomDns {
  std::string type;  // sing-box's: udp, tcp, tls, https, quic
  std::string host;  // an address (IPv6 without brackets) or a name
  int port = 53;
  std::string path;  // https: "/dns-query"
  bool named = false;  // `host` a name: resolved first, by the system's resolver
};
std::optional<CustomDns> ParseCustomDns(std::string_view text);

// The servers WARP may go out through (kWarpViaTag in `config`), in their
// order - the first is the one it starts on; empty when it goes directly.
std::vector<std::string> WarpViaServers(std::string_view config);

// Whether a core log line says WARP's tunnel isn't getting through: a
// connection through it timed out, or WireGuard couldn't reach its server.
bool WarpStalled(std::string_view line);

}  // namespace sovereign::tray
