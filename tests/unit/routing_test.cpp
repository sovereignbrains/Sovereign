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

void TestJsonAndParse() {
  RoutingSettings s;
  s.source = RoutingSettings::Source::Profile;
  s.sourceProfile = "p1";
  s.russiaDirect = false;
  s.remoteDns = RoutingSettings::RemoteDns::Quad9;
  s.localDns = RoutingSettings::LocalDns::System;
  s.rules.push_back(*ParseRule("qwen.ai ~tracker 10.0.0.0/8 App.exe", RouteRule::Action::Block));
  const RoutingSettings back = RoutingFromJson(json::parse(RoutingToJson(s).dump()));
  CHECK(back.source == RoutingSettings::Source::Profile && back.sourceProfile == "p1");
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
  CHECK(c["dns"]["rules"][0]["server"] == "sov-local");  // qwen.ai resolved directly
  // Blocked names: "no such domain" - REFUSED made Windows retry for ~11 s.
  int nxdomain = 0;
  for (const json& rule : c["dns"]["rules"]) {
    CHECK(rule.value("action", "") != "reject");
    nxdomain += rule.value("action", "") == "predefined" && rule.value("rcode", "") == "NXDOMAIN" ? 1 : 0;
  }
  CHECK(nxdomain == (Files().empty() ? 1 : 2));  // the user's block rule, and the ads list when it's there
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

void TestProfile() {
  RoutingSettings s;
  s.source = RoutingSettings::Source::Profile;
  s.rules.push_back(*ParseRule("my.example", RouteRule::Action::Direct));
  const json c = json::parse(ApplyRouting(kSubscription, s, Files()));
  Checked().emplace_back("profile", c.dump());
  const json& rules = c["route"]["rules"];
  // Ours first, after its sniffing, hijacking and local network; its own after ours.
  CHECK(rules[3]["domain_suffix"] == json::array({"my.example"}));
  CHECK(rules.dump().find("qwen.ai") > rules.dump().find("xn--p1ai"));
  CHECK(c["route"]["final"] == "proxy");     // its own
  CHECK(!c["dns"].contains("strategy"));     // its own
  CHECK(c["dns"]["rules"].back()["domain_suffix"] == json::array({"qwen.ai"}));  // its DNS rules after ours
  bool local = false;
  for (const json& server : c["dns"]["servers"]) {
    local = local || server["tag"] == "sov-local";
  }
  CHECK(local);

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
  s.source = RoutingSettings::Source::Profile;
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

// WARP: its endpoint over the proxy, rules "через WARP" to it and nothing
// else; off, none of it - and the rules wait.
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
    CHECK(c["endpoints"][0]["type"] == "wireguard" && c["endpoints"][0]["tag"] == "warp" &&
          c["endpoints"][0]["detour"] == "proxy");
  }
  // Only its sites go to WARP - no IPv6 of everything (that made WARP global).
  bool rule = false;
  for (const json& r : c["route"]["rules"]) {
    rule = rule || (r.value("outbound", "") == "warp" && r.dump().find("chatgpt.com") != std::string::npos);
    CHECK(!r.contains("ip_version"));
  }
  CHECK(rule);
  // "Только IPv4" stays what it says, WARP or not.
  CHECK(c["dns"]["strategy"] == "ipv4_only");

  // Directly: no detour.
  s.warpViaProxy = false;
  const json d = json::parse(ApplyRouting(on.config.value_or("{}"), s, Files()));
  CHECK(!d["endpoints"][0].contains("detour"));

  // tray.json keeps it all.
  const RoutingSettings back = RoutingFromJson(json::parse(RoutingToJson(s).dump()));
  CHECK(back.warp && !back.warpViaProxy && back.warpAccount.has_value() &&
        back.rules.size() == 1 && back.rules[0].action == RouteRule::Action::Warp);
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
    TestProfile();
    TestWarp();
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
