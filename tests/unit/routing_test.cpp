// The client's routing and DNS (src/tray/routing.h) - the own frame, the
// rules first in whatever config runs, Russia directly. With `--check
// <sing-box.exe>` the configs go through the pinned sing-box's `check`, the
// lists compiled by it from small sources (ctest routing-configs-singbox-check).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "check.h"
#include "combine.h"
#include "routing.h"
#include "share_links.h"

namespace {

using json = nlohmann::json;
using namespace sovereign::tray;

// A subscription's config with routing of its own, as packetlab's.
constexpr std::string_view kSubscription = R"({
  "dns": {"servers": [{"type": "tls", "tag": "remote", "server": "1.1.1.1", "detour": "proxy"},
                      {"type": "local", "tag": "local"}],
          "rules": [{"domain_suffix": ["qwen.ai"], "server": "local"}], "final": "remote"},
  "inbounds": [{"type": "tun", "tag": "tun-in", "address": ["172.19.0.1/30"], "auto_route": true}],
  "outbounds": [
    {"type": "selector", "tag": "proxy", "outbounds": ["auto", "NL"]},
    {"type": "urltest", "tag": "auto", "outbounds": ["NL"]},
    {"type": "trojan", "tag": "NL", "server": "nl.example.com", "server_port": 443, "password": "p", "tls": {"enabled": true}},
    {"type": "direct", "tag": "direct"}],
  "route": {"rules": [{"action": "sniff"}, {"protocol": "dns", "action": "hijack-dns"},
                      {"ip_is_private": true, "outbound": "direct"},
                      {"domain_suffix": ["qwen.ai", "alicdn.com"], "outbound": "direct"},
                      {"domain": ["exact.example.com"], "domain_keyword": ["tracker"], "action": "reject"},
                      {"ip_cidr": ["10.9.0.0/16"], "outbound": "proxy"},
                      {"rule_set": "ads", "action": "reject"},
                      {"protocol": "quic", "action": "reject"}],
            "final": "proxy", "auto_detect_interface": true, "default_domain_resolver": "local"}
})";

std::string Keys() {
  return BuildConfigFromLinks({"trojan://pw@nl2.example.com:443#NL", "trojan://pw@de.example.com:443#DE"})
      .config.value_or("{}");
}

std::vector<std::pair<std::string, std::string>>& Checked() {
  static std::vector<std::pair<std::string, std::string>> checked;
  return checked;
}

std::map<std::string, std::string>& Files() {
  static std::map<std::string, std::string> files;
  return files;
}

// The DNS rules that ask a server, in order.
std::vector<json> Asked(const json& dns) {
  std::vector<json> out;
  for (const json& rule : dns["rules"]) {
    if (rule.value("action", "") == "evaluate") {
      out.push_back(rule);
    }
  }
  return out;
}

void TestJsonAndParse() {
  RoutingSettings s;
  s.russiaDirect = false;
  s.remoteDns = RoutingSettings::RemoteDns::Quad9;
  s.localDns = RoutingSettings::LocalDns::System;
  s.rules.push_back(*ParseRule("qwen.ai ~tracker 10.0.0.0/8 App.exe", RouteRule::Action::Block));
  const RoutingSettings back = RoutingFromJson(json::parse(RoutingToJson(s).dump()));
  // A tray.json from before 0.4.29 with "source": "profile": read, the field ignored.
  CHECK(RoutingFromJson(json{{"source", "profile"}, {"sourceProfile", "p1"}, {"blockAds", false}}).blockAds == false);
  CHECK(!back.russiaDirect && back.blockAds && back.remoteDns == RoutingSettings::RemoteDns::Quad9);
  CHECK(back.localDns == RoutingSettings::LocalDns::System);
  CHECK(RoutingSettings{}.localDns == RoutingSettings::LocalDns::Cloudflare);  // encrypted, directly, by default
  CHECK(back.rules.size() == 1 && back.rules[0].action == RouteRule::Action::Block);
  CHECK(!back.rules.empty() && back.rules[0].keywords == std::vector<std::string>({"tracker"}));
  CHECK(RoutingFromJson(json::parse(R"({"russiaDirect":"yes","rules":[5,{"domains":[1]}]})")).russiaDirect);
  CHECK(RoutingFromJson(json("x")).blockQuic);

  const auto rule = ParseRule("https://WWW.Example.com/path, *.cdn.example.net; .ru 1.2.3.4 2001:db8::/32 Telegram.exe ~ads",
                              RouteRule::Action::Direct);
  CHECK(rule.has_value());
  if (rule) {
    CHECK(rule->domains == std::vector<std::string>({"www.example.com", "cdn.example.net", "ru"}));
    CHECK(rule->ips == std::vector<std::string>({"1.2.3.4/32", "2001:db8::/32"}));
    CHECK(rule->processes == std::vector<std::string>({"Telegram.exe"}));
    CHECK(rule->keywords == std::vector<std::string>({"ads"}));
    CHECK(RuleText(*rule) == "www.example.com, cdn.example.net, ru, ~ads, 1.2.3.4/32, 2001:db8::/32, Telegram.exe");
  }
  CHECK(!ParseRule("  ,; ", RouteRule::Action::Direct));
  CHECK(!ParseRule("!!! ?? @@", RouteRule::Action::Direct));  // nothing a name, an address or a program
  const auto hex = ParseRule("cafe.de", RouteRule::Action::Direct);
  CHECK(hex.has_value() && hex->domains.size() == 1);  // hex letters, still a name
}

void TestImport() {
  const auto rules = ImportRules(kSubscription);
  CHECK(rules.size() == 3);
  if (rules.size() == 3) {
    CHECK(rules[0].action == RouteRule::Action::Direct &&
          rules[0].domains == std::vector<std::string>({"qwen.ai", "alicdn.com"}));
    CHECK(rules[1].action == RouteRule::Action::Block && rules[1].domains == std::vector<std::string>({"exact.example.com"}) &&
          rules[1].keywords == std::vector<std::string>({"tracker"}));
    CHECK(rules[2].action == RouteRule::Action::Proxy && rules[2].ips == std::vector<std::string>({"10.9.0.0/16"}));
  }
  std::vector<RouteRule> merged = rules;
  MergeRules(merged, rules);
  CHECK(merged.size() == 3);
  CHECK(ImportRules("not json").empty());
}

void TestOwn() {
  RoutingSettings s;
  s.rules.push_back(*ParseRule("qwen.ai", RouteRule::Action::Direct));
  s.rules.push_back(*ParseRule("blocked.example", RouteRule::Action::Block));
  s.rules.push_back(*ParseRule("10.9.0.0/16", RouteRule::Action::Proxy));
  const auto combined = CombineConfigs({{"Ключи", Keys(), {"DE"}}, {"Подписка", std::string(kSubscription), {}}},
                                       OwnFrame(s));
  CHECK(combined.config.has_value());
  const std::string text = ApplyRouting(combined.config.value_or("{}"), s, Files());
  Checked().emplace_back("own", text);
  const json c = json::parse(text);
  // Only servers from the configurations: theirs, not their routing.
  std::vector<std::string> members;
  for (const json& o : c["outbounds"]) {
    if (o["tag"] == "proxy") {
      members = o["outbounds"].get<std::vector<std::string>>();
    }
  }
  CHECK(members == std::vector<std::string>({"auto", "NL", "NL · Подписка"}));
  CHECK(c["inbounds"].size() == 1 && c["inbounds"][0]["stack"] == "mixed");
  const json& rules = c["route"]["rules"];
  CHECK(rules[0]["action"] == "sniff" && rules[1]["action"] == "hijack-dns" && rules[2].contains("ip_is_private"));
  CHECK(rules[3]["domain_suffix"] == json::array({"qwen.ai"}) && rules[3]["outbound"] == "direct");  // the user's first
  CHECK(rules[4]["action"] == "reject");
  CHECK(rules[5]["outbound"] == "proxy");
  const std::string dump = rules.dump();
  CHECK(dump.find("xn--p1ai") != std::string::npos);  // .рф directly
  CHECK(rules.back()["protocol"] == "quic");
  CHECK(c["route"]["final"] == "proxy");
  CHECK(c["dns"]["strategy"] == "ipv4_only");
  // The servers' own names directly, first: through the proxy a server's name
  // (its ECH config too) would need that very server.
  const std::vector<json> asked = Asked(c["dns"]);
  CHECK(asked.size() >= 3 && asked[0]["server"] == "sov-local");
  CHECK(asked[0]["domain"] == json::array({"nl.example.com", "nl2.example.com"}));
  CHECK(asked[1]["server"] == "sov-local");  // qwen.ai resolved directly
  CHECK(asked.back()["server"] == "remote" && asked.back().size() == 2);  // everything else
  // Blocked names: "no addresses" - REFUSED and NXDOMAIN both made Windows wait ~11 s.
  int noAddress = 0;
  for (const json& rule : c["dns"]["rules"]) {
    CHECK(rule.value("action", "") != "reject");
    noAddress += rule.value("action", "") == "predefined" && rule.value("rcode", "") == "NOERROR" && !rule.contains("answer") &&
                         !rule.contains("match_response")
                     ? 1
                     : 0;
  }
  CHECK(noAddress == (Files().empty() ? 1 : 2));  // the user's block rule, and the ads list when it's there
  if (!Files().empty()) {
    CHECK(c["route"]["rule_set"].size() == 4);
    CHECK(dump.find("sov-geoip-ru") != std::string::npos);
  }

  // Everything else directly, Google for the direct names, IPv6 allowed.
  s.finalDirect = true;
  s.localDns = RoutingSettings::LocalDns::Google;
  s.ipv4Only = false;
  const auto direct = CombineConfigs({{"Ключи", Keys(), {}}}, OwnFrame(s));
  const json d = json::parse(ApplyRouting(direct.config.value_or("{}"), s, Files()));
  Checked().emplace_back("own-direct", d.dump());
  CHECK(d["route"]["final"] == "direct");
  CHECK(!d["dns"].contains("strategy"));
  CHECK(d["dns"]["servers"][1]["server"] == "8.8.8.8" && d["dns"]["servers"][1]["type"] == "https");

  // The lists not downloaded yet: the zones still go directly, nothing refers to a missing list.
  RoutingSettings bare;
  const json b = json::parse(ApplyRouting(CombineConfigs({{"Ключи", Keys(), {}}}, OwnFrame(bare)).config.value_or("{}"),
                                          bare, {}));
  Checked().emplace_back("own-no-lists", b.dump());
  CHECK(!b["route"].contains("rule_set"));
  CHECK(b["route"]["rules"].dump().find("sov-") == std::string::npos);
  CHECK(b["route"]["rules"].dump().find("xn--p1ai") != std::string::npos);
}

// A subscription's own routing and DNS (packetlab's: qwen.ai directly, its
// "ads" list rejected, DNS over TLS through the proxy) never get in: from a
// configuration only its servers - routing and DNS are the client's.
void TestSubscriptionRulesDropped() {
  RoutingSettings s;
  s.rules.push_back(*ParseRule("my.example", RouteRule::Action::Direct));
  const auto combined = CombineConfigs({{"sub", std::string(kSubscription), {}}}, OwnFrame(s));
  const json c = json::parse(ApplyRouting(combined.config.value_or("{}"), s, Files()));
  Checked().emplace_back("subscription", c.dump());
  CHECK(c["route"]["rules"].dump().find("alicdn.com") == std::string::npos);
  CHECK(c["route"]["rules"].dump().find("\"ads\"") == std::string::npos);
  CHECK(c["route"]["rules"].dump().find("my.example") != std::string::npos);
  CHECK(c["dns"]["rules"].dump().find("qwen.ai") == std::string::npos);
  for (const json& server : c["dns"]["servers"]) {
    CHECK(server["type"] != "tls");  // its DNS server: gone
  }
  CHECK(c["route"]["final"] == "proxy" && c["dns"]["strategy"] == "ipv4_only");  // ours
  bool nl = false;
  for (const json& o : c["outbounds"]) {
    nl = nl || o.value("tag", "").starts_with("NL");
  }
  CHECK(nl);  // its server is there

  // A config with no direct outbound and no DNS: they're added.
  const json bare = json::parse(ApplyRouting(R"({"outbounds":[{"type":"selector","tag":"p","outbounds":["x"]},)"
                                             R"({"type":"trojan","tag":"x","server":"a","server_port":1,"password":"p"}],)"
                                             R"("route":{"final":"p"}})",
                                             s, {}));
  bool direct = false;
  for (const json& o : bare["outbounds"]) {
    direct = direct || o["type"] == "direct";
  }
  CHECK(direct && bare["dns"]["servers"].size() == 1);
  CHECK(ApplyRouting("not json", s, {}) == "not json");
}

// Typos: NXDOMAIN made Windows wait ~11 s for the Wi-Fi's DNS. Each server
// asked, then NXDOMAIN -> no addresses, then its answer under the same
// condition (a failed query fails, never falls to the next server).
void TestTypos() {
  RoutingSettings s;
  const json c = json::parse(ApplyRouting(kSubscription, s, Files()));
  const json& rules = c["dns"]["rules"];
  for (std::size_t i = 0; i < rules.size(); ++i) {
    const json& ask = rules[i];
    if (ask["action"] != "evaluate") {
      CHECK(ask["action"] == "predefined" && !ask.contains("match_response"));  // a blocked name
      continue;
    }
    CHECK(ask.contains("server") && i + 2 < rules.size());
    if (i + 2 >= rules.size()) {
      break;
    }
    CHECK(rules[i + 1] ==
          json({{"match_response", true}, {"response_rcode", "NXDOMAIN"}, {"action", "predefined"}, {"rcode", "NOERROR"}}));
    json condition = ask;
    condition.erase("server");
    condition["action"] = "respond";
    CHECK(rules[i + 2] == condition);
    i += 2;
  }
  CHECK(rules.back() == json({{"action", "respond"}}));

  // Rules sing-box reads the old way can't sit next to evaluate: left as they are.
  const std::string old = R"({"dns": {"servers": [{"type": "local", "tag": "local"}],
                                      "rules": [{"ip_cidr": ["10.0.0.0/8"], "server": "local"}]},
                              "outbounds": [{"type": "direct", "tag": "direct"}],
                              "route": {"default_domain_resolver": "local"}})";
  const json o = json::parse(ApplyRouting(old, s, {}));
  Checked().emplace_back("typos-old-style", o.dump());
  CHECK(Asked(o["dns"]).empty() && o["dns"]["rules"].back()["ip_cidr"] == json::array({"10.0.0.0/8"}));

  // Fakeip routes stay routes; the rest is asked.
  const std::string fake = R"({"dns": {"servers": [{"type": "local", "tag": "local"},
                                                   {"type": "fakeip", "tag": "fake", "inet4_range": "198.18.0.0/15"}],
                                       "rules": [{"domain_suffix": ["fake.example"], "server": "fake"}],
                                       "final": "local"},
                               "outbounds": [{"type": "direct", "tag": "direct"}],
                               "route": {"default_domain_resolver": "local"}})";
  const json f = json::parse(ApplyRouting(fake, s, {}));
  Checked().emplace_back("typos-fakeip", f.dump());
  CHECK(f["dns"]["rules"].dump().find(R"("server":"fake")") != std::string::npos);
  CHECK(Asked(f["dns"]).size() == 2 && Asked(f["dns"]).back()["server"] == "local");

  // The configuration's own rule set (packetlab's "ads"): what's in it can't
  // be told here - laid out all the same, the core decides.
  const std::string theirs = R"({"dns": {"servers": [{"type": "local", "tag": "local"}],
                                         "rules": [{"rule_set": "theirs", "action": "reject"}]},
                                 "outbounds": [{"type": "direct", "tag": "direct"}],
                                 "route": {"default_domain_resolver": "local"}})";
  CHECK(Asked(json::parse(ApplyRouting(theirs, s, {}))["dns"]).size() == 2);
}

int Run(const std::wstring& command) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  DWORD code = 1;
  std::wstring line = command;
  if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  return static_cast<int>(code);
}

// Lists for the check: compiled by the pinned sing-box from small sources.
bool MakeLists(const std::filesystem::path& singBox, const std::filesystem::path& dir) {
  const std::map<std::string, std::string> sources = {
      {"sov-geosite-ru", R"({"version":3,"rules":[{"domain_suffix":["vk.com","yandex.net"]}]})"},
      {"sov-geosite-gov-ru", R"({"version":3,"rules":[{"domain_suffix":["gosuslugi.ru"]}]})"},
      {"sov-geoip-ru", R"({"version":3,"rules":[{"ip_cidr":["5.0.0.0/16"]}]})"},
      {"sov-ads", R"({"version":3,"rules":[{"domain_suffix":["doubleclick.net"]}]})"},
      {"sov-svc-netflix", R"({"version":3,"rules":[{"domain_suffix":["netflix.com","nflxvideo.net"]}]})"},
  };
  for (const auto& [tag, source] : sources) {
    const auto in = dir / (tag + ".json");
    const auto out = dir / (tag + ".srs");
    std::ofstream(in, std::ios::binary) << source;
    if (Run(L"\"" + singBox.wstring() + L"\" rule-set compile \"" + in.wstring() + L"\" -o \"" + out.wstring() + L"\"") != 0) {
      return false;
    }
    Files()[tag] = out.string();
  }
  return true;
}

}  // namespace

// `--try <sing-box.exe> <lists dir> <config.json>...`: real configurations,
// real lists (<tag>.srs in the dir), own mode and each one's routing - all
// through sing-box's check. Prints names and results only.
int TryReal(int argc, char** argv) {
  const std::filesystem::path singBox = argv[2];
  const std::filesystem::path lists = argv[3];
  RoutingSettings s;
  for (const RuleSetSource& source : NeededRuleSets(s)) {
    if (std::filesystem::exists(lists / (source.tag + ".srs"))) {
      Files()[source.tag] = (lists / (source.tag + ".srs")).string();
    }
  }
  std::vector<ProfileConfig> parts;
  for (int i = 4; i < argc; ++i) {
    std::ifstream in(std::filesystem::path(argv[i]), std::ios::binary);
    parts.push_back({.name = "c" + std::to_string(i), .config = {std::istreambuf_iterator<char>(in), {}}, .disabled = {}});
    for (const RouteRule& rule : ImportRules(parts.back().config)) {
      MergeRules(s.rules, {rule});
    }
  }
  std::cout << "lists: " << Files().size() << ", imported rules: " << s.rules.size() << "\n";
  const auto own = CombineConfigs(parts, OwnFrame(s));
  Checked().emplace_back("real-own", ApplyRouting(own.config.value_or("{}"), s, Files()));
  for (std::size_t i = 0; i < parts.size(); ++i) {
    std::vector<ProfileConfig> ordered = parts;
    std::rotate(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(i), ordered.end());
    const auto combined = CombineConfigs(ordered);
    Checked().emplace_back("real-profile-" + std::to_string(i), ApplyRouting(combined.config.value_or("{}"), s, Files()));
  }
  const auto dir = std::filesystem::temp_directory_path() / "sovereign-routing-check";
  std::filesystem::create_directories(dir);
  int failed = 0;
  for (const auto& [name, config] : Checked()) {
    const auto path = dir / (name + ".json");
    std::ofstream(path, std::ios::binary) << config;
    const int code = Run(L"\"" + singBox.wstring() + L"\" check -c \"" + path.wstring() + L"\"");
    std::cout << (code == 0 ? "ok     " : "FAILED ") << name << "\n";
    failed += code == 0 ? 0 : 1;
  }
  return failed == 0 ? 0 : 1;
}

// Naive with "insecure" (a Karing export): the official core refuses the whole
// config for it - gone, the rest of its TLS kept, and sing-box check passes.
void TestNaiveInsecure() {
  const RoutingSettings s;
  const std::string naive = BuildConfigFromLinks({"trojan://pw@nl2.example.com:443#NL"}).config.value_or("{}");
  json withNaive = json::parse(naive);
  withNaive["outbounds"].push_back(json::parse(
      R"({"type":"naive","tag":"Naive","server":"cloud.example.com","server_port":443,"username":"u","password":"p",)"
      R"("tls":{"enabled":true,"server_name":"cloud.example.com","insecure":true}})"));
  const auto combined = CombineConfigs({{"ключи", withNaive.dump(), {}}}, OwnFrame(s));
  const std::string text = ApplyRouting(combined.config.value_or("{}"), s, Files());
  Checked().emplace_back("naive", text);
  bool found = false;
  for (const json& o : json::parse(text)["outbounds"]) {
    if (o.value("type", "") == "naive") {
      found = true;
      CHECK(!o["tls"].contains("insecure"));
      CHECK(o["tls"].value("server_name", "") == "cloud.example.com");
    }
  }
  CHECK(found);
}

// WARP: its endpoint over the proxy, rules "через WARP" to it and nothing
// else; off, none of it - and the rules wait.
// Services: Netflix through WARP by its list - kept in tray.json, its list
// downloaded, WARP's endpoint there for it alone, the rule after the user's.
void TestServices() {
  RoutingSettings s;
  s.services.push_back({.id = "netflix", .way = ServiceRoute::Way::Warp, .country = {}});
  const RoutingSettings back = RoutingFromJson(json::parse(RoutingToJson(s).dump()));
  CHECK(back.services.size() == 1 && back.services[0].id == "netflix");
  CHECK(RoutingFromJson(json{{"services", json::array({json{{"id", "nope"}}, json{{"id", "netflix"}}, json{{"id", "netflix"}}})}})
            .services.size() == 1);  // unknown and repeated ones dropped
  bool list = false;
  for (const RuleSetSource& set : NeededRuleSets(s)) {
    list = list || (set.tag == "sov-svc-netflix" && set.url.ends_with("/geosite-netflix.srs"));
  }
  CHECK(list);
  CHECK(FindService("chatgpt") != nullptr && FindService("chatgpt")->geosite == "openai");
  CHECK(ServiceCatalog().size() >= 10);

  WarpAccount account;
  account.privateKey = "YJ9Lx3UFbVd6R+1QpYx0K5nV5+OYn5cB4m3Hk4qH0lI=";
  account.peerKey = "bmXOC+F1FxEMF9dyiK2H5/1SUtzH0JuVo51h2wPfgyo=";
  account.address4 = "172.16.0.2";
  s.warpAccount = account;
  s.warp = true;
  CHECK(WarpReady(s));  // a service alone is reason enough for WARP
  s.rules.push_back(*ParseRule("my.example", RouteRule::Action::Direct));
  const auto combined = CombineConfigs({{"keys", Keys(), {}}}, OwnFrame(s));
  const std::string text = ApplyRouting(combined.config.value_or("{}"), s, Files());
  Checked().emplace_back("services", text);
  const json c = json::parse(text);
  CHECK(c.contains("endpoints") && c["endpoints"].size() == 1);
  if (!Files().empty()) {
    int mine = -1;
    int netflix = -1;
    for (std::size_t i = 0; i < c["route"]["rules"].size(); ++i) {
      const json& r = c["route"]["rules"][i];
      mine = r.dump().find("my.example") != std::string::npos ? static_cast<int>(i) : mine;
      netflix = r.value("rule_set", json()) == "sov-svc-netflix" && r.value("outbound", "") == "warp" ? static_cast<int>(i) : netflix;
    }
    CHECK(netflix > mine && mine >= 0);  // after the user's own rules
  }
}

// A service through a country: its own selector (the main group by default,
// then the servers), switched by the tray to the fastest server measured
// there; none there - as usual.
void TestServiceCountry() {
  RoutingSettings s;
  s.services.push_back({.id = "netflix", .way = ServiceRoute::Way::Country, .country = "US"});
  const RoutingSettings back = RoutingFromJson(json::parse(RoutingToJson(s).dump()));
  CHECK(back.services.size() == 1 && back.services[0].way == ServiceRoute::Way::Country &&
        back.services[0].country == "US");
  CHECK(RoutingFromJson(json{{"services", json::array({json{{"id", "netflix"}, {"way", "country"}, {"country", "usa"}}})}})
            .services[0]
            .way == ServiceRoute::Way::Warp);  // not a country code: WARP, as before
  CHECK(!WarpReady(s));  // through a country: no WARP needed for it

  const auto combined = CombineConfigs({{"keys", Keys(), {}}}, OwnFrame(s));
  const std::string text = ApplyRouting(combined.config.value_or("{}"), s, Files());
  Checked().emplace_back("service-country", text);
  const json c = json::parse(text);
  if (!Files().empty()) {
    json selector;
    for (const json& out : c["outbounds"]) {
      selector = out["tag"] == ServiceOutboundTag("netflix") ? out : selector;
    }
    CHECK(selector["type"] == "selector" && selector["default"] == "proxy");
    CHECK(selector["outbounds"] == json::array({"proxy", "NL", "DE"}));
    bool rule = false;
    for (const json& r : c["route"]["rules"]) {
      rule = rule || (r.value("rule_set", json()) == "sov-svc-netflix" && r.value("outbound", "") == "sov-out-netflix");
    }
    CHECK(rule);
  }

  const std::vector<std::string> servers = {"A", "B", "C", "D"};
  const std::map<std::string, std::string> exits = {{"A", "DE"}, {"B", "US"}, {"C", "US"}, {"D", "US"}};
  CHECK(PickCountryServer("US", servers, exits, {{"B", 180}, {"C", 90}}) == "C");  // the fastest there
  CHECK(PickCountryServer("US", servers, exits, {{"D", 300}}) == "D");             // measured beats not
  CHECK(PickCountryServer("US", servers, exits, {}) == "B");                       // none measured: the first
  CHECK(PickCountryServer("JP", servers, exits, {}).empty());                      // none there
  CHECK(CountryName("NL") == "Нидерланды" && CountryName("XQ") == "XQ");
  CHECK(IsCountryCode("US") && !IsCountryCode("us") && !IsCountryCode("USA"));
}

void TestWarp() {
  RoutingSettings s;
  s.rules.push_back(*ParseRule("chatgpt.com", RouteRule::Action::Warp));
  WarpAccount account;
  account.privateKey = "YJ9Lx3UFbVd6R+1QpYx0K5nV5+OYn5cB4m3Hk4qH0lI=";
  account.peerKey = "bmXOC+F1FxEMF9dyiK2H5/1SUtzH0JuVo51h2wPfgyo=";
  account.address4 = "172.16.0.2";
  account.address6 = "2606:4700:110:87bf:c0a0:2c80:262b:5924";
  account.reserved = {0x21, 0xEE, 0x10};
  s.warpAccount = account;

  // Off: no endpoint, the rule left out.
  const auto off = CombineConfigs({{"ключи", Keys(), {}}}, OwnFrame(s));
  const json o = json::parse(ApplyRouting(off.config.value_or("{}"), s, Files()));
  CHECK(!o.contains("endpoints"));
  CHECK(o["route"]["rules"].dump().find("chatgpt.com") == std::string::npos);
  CHECK(o["dns"]["strategy"] == "ipv4_only");

  s.warp = true;
  const auto on = CombineConfigs({{"ключи", Keys(), {}}}, OwnFrame(s));
  const std::string text = ApplyRouting(on.config.value_or("{}"), s, Files());
  Checked().emplace_back("warp", text);
  const json c = json::parse(text);
  CHECK(c["endpoints"].size() == 1);
  if (c["endpoints"].size() == 1) {
    // Over the proxy through a group of its own: a switch of the selector moved
    // WireGuard's UDP and WARP went quiet until its next handshake (15-25 s).
    CHECK(c["endpoints"][0]["type"] == "wireguard" && c["endpoints"][0]["tag"] == "warp" &&
          c["endpoints"][0]["detour"] == "warp-via");
  }
  json via;
  json selector;
  for (const json& out : c["outbounds"]) {
    via = out["tag"] == "warp-via" ? out : via;
    selector = out["tag"] == "proxy" ? out : selector;
  }
  CHECK(via["type"] == "selector");  // the tray picks its server by WARP answering
  CHECK(via["outbounds"] == json::array({"NL", "DE"}));  // the servers, "auto" expanded, each once
  CHECK(WarpViaServers(text) == std::vector<std::string>({"NL", "DE"}));
  CHECK(selector["outbounds"].dump().find("warp-via") == std::string::npos);  // not a choice for the user
  // Only its sites go to WARP - no IPv6 of everything (that made WARP global).
  bool rule = false;
  for (const json& r : c["route"]["rules"]) {
    rule = rule || (r.value("outbound", "") == "warp" && r.dump().find("chatgpt.com") != std::string::npos);
    CHECK(!r.contains("ip_version"));
  }
  CHECK(rule);
  // "Только IPv4" stays what it says, WARP or not.
  CHECK(c["dns"]["strategy"] == "ipv4_only");

  // On, but no site sent to it: no endpoint - a tunnel nobody uses isn't kept up.
  RoutingSettings unused = s;
  unused.rules.clear();
  const json idle = json::parse(ApplyRouting(on.config.value_or("{}"), unused, Files()));
  CHECK(!idle.contains("endpoints") || idle["endpoints"].empty());

  // Directly: no detour.
  s.warpViaProxy = false;
  const json d = json::parse(ApplyRouting(on.config.value_or("{}"), s, Files()));
  CHECK(!d["endpoints"][0].contains("detour"));

  // tray.json keeps it all.
  const RoutingSettings back = RoutingFromJson(json::parse(RoutingToJson(s).dump()));
  CHECK(back.warp && !back.warpViaProxy && back.warpAccount.has_value() &&
        back.rules.size() == 1 && back.rules[0].action == RouteRule::Action::Warp);
}
// The local DNS giving way when it stops answering: in what order, and what
// in the core's log says it doesn't (07.10.2026: 8.8.8.8:443 stopped
// answering from a home ISP and the box resolved nothing).
void TestLocalDnsFallback() {
  using LocalDns = RoutingSettings::LocalDns;
  CHECK(LocalDnsOrder(LocalDns::Google) ==
        (std::vector{LocalDns::Google, LocalDns::Cloudflare, LocalDns::Quad9, LocalDns::System}));
  CHECK(LocalDnsOrder(LocalDns::Quad9).front() == LocalDns::Quad9 && LocalDnsOrder(LocalDns::Quad9).back() == LocalDns::System);
  // The system's picked: it first (a failing system resolver doesn't say an address, so it's never left).
  CHECK(LocalDnsOrder(LocalDns::System) ==
        (std::vector{LocalDns::System, LocalDns::Cloudflare, LocalDns::Google, LocalDns::Quad9}));
  CHECK(LocalDnsAddress(LocalDns::Google) == "8.8.8.8" && LocalDnsAddress(LocalDns::System).empty());
  CHECK(LocalDnsName(LocalDns::Quad9) == "Quad9");

  // The lines the box wrote that evening.
  CHECK(LocalDnsUnreachable("ERROR[0101] outbound/urltest[auto]: lookup main.spacevpn.com: dial tcp 8.8.8.8:443: "
                            "i/o timeout",
                            "8.8.8.8"));
  CHECK(LocalDnsUnreachable("dns: exchange failed for x. IN A: dial tcp 1.1.1.1:443: connectex: No connection could "
                            "be made because the target machine actively refused it.",
                            "1.1.1.1"));
  // Not that server, or not a dial to it.
  CHECK(!LocalDnsUnreachable("lookup main.spacevpn.com: dial tcp 8.8.8.8:443: i/o timeout", "1.1.1.1"));
  CHECK(!LocalDnsUnreachable("dial tcp 18.8.8.8:443: i/o timeout", "8.8.8.8"));
  CHECK(!LocalDnsUnreachable("dns: exchange failed for google.ru. IN A: context deadline exceeded", "8.8.8.8"));
  CHECK(!LocalDnsUnreachable("outbound/direct: connected to 8.8.8.8:443", "8.8.8.8"));
  CHECK(!LocalDnsUnreachable("dial tcp 8.8.8.8:443: i/o timeout", ""));

  // Quad9 is a pick of its own, kept in tray.json; its server is its address.
  RoutingSettings s;
  s.localDns = LocalDns::Quad9;
  CHECK(RoutingFromJson(json::parse(RoutingToJson(s).dump())).localDns == LocalDns::Quad9);
  const json applied = json::parse(ApplyRouting(R"({"outbounds":[{"type":"direct","tag":"direct"}]})", s, {}));
  bool quad9 = false;
  for (const json& server : applied["dns"]["servers"]) {
    quad9 = quad9 || (server.value("tag", "") == "sov-local" && server.value("server", "") == "9.9.9.9");
  }
  CHECK(quad9);
}

// WARP's tunnel not getting through, as the core logs it; its servers when it
// goes directly - none.
void TestWarpWatch() {
  CHECK(WarpStalled("ERROR[0412] [1234 30.0s] connection: open connection to github.com:443 using "
                    "endpoint/wireguard[warp]: connect tcp 140.82.121.3:443: operation timed out"));
  CHECK(WarpStalled("ERROR endpoint/wireguard[warp]: peer(bmXO.fgyo) - failed to send handshake initiation: "
                    "no known endpoint for peer"));
  CHECK(WarpStalled("ERROR endpoint/wireguard[warp]: connect to server: failed to create session: dial tcp "
                    "5.83.147.210:443: i/o timeout"));
  CHECK(!WarpStalled("ERROR endpoint/wireguard[warp]: read packet: use of closed network connection"));
  CHECK(!WarpStalled("ERROR outbound/vless[NL]: dial tcp 1.2.3.4:443: i/o timeout"));
  CHECK(WarpViaServers(R"({"outbounds":[{"type":"direct","tag":"direct"}]})").empty());
  CHECK(WarpViaServers("not json").empty());
  CHECK(WarpViaServers(R"({"outbounds":[5,{"tag":7},{"tag":"warp-via","outbounds":["a",3,"b"]}]})") ==
        std::vector<std::string>({"a", "b"}));
}

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  if (argc >= 5 && std::string_view(argv[1]) == "--try") {
    return TryReal(argc, argv);
  }
  try {
    const bool check = argc == 3 && std::string_view(argv[1]) == "--check";
    const auto dir = std::filesystem::temp_directory_path() / "sovereign-routing-check";
    if (check) {
      std::filesystem::create_directories(dir);
      if (!MakeLists(argv[2], dir)) {
        std::cerr << "sing-box rule-set compile failed\n";
        return 1;
      }
    }
    TestJsonAndParse();
    TestImport();
    TestOwn();
    TestSubscriptionRulesDropped();
    TestTypos();
    TestServices();
    TestServiceCountry();
    TestWarp();
    TestNaiveInsecure();
    TestLocalDnsFallback();
    TestWarpWatch();
    if (check) {
      int failed = 0;
      for (const auto& [name, config] : Checked()) {
        const auto path = dir / (name + ".json");
        std::ofstream(path, std::ios::binary) << config;
        const int code = Run(L"\"" + std::filesystem::path(argv[2]).wstring() + L"\" check -c \"" + path.wstring() + L"\"");
        std::cout << (code == 0 ? "ok     " : "FAILED ") << name << "\n";
        failed += code == 0 ? 0 : 1;
      }
      if (failed != 0) {
        return 1;
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
