// Several configurations on at once, servers switched off
// (src/tray/combine.h). With `--check <sing-box.exe>` the combined configs
// also go through the pinned sing-box's `check` (ctest
// combine-configs-singbox-check).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "check.h"
#include "combine.h"
#include "share_links.h"

namespace {

using nlohmann::ordered_json;
using namespace sovereign::tray;
using json = nlohmann::json;

constexpr std::string_view kKey = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE";

// A subscription's config the way panels write them: a selector with "auto",
// rules and DNS through particular servers, a rule set downloaded through one.
std::string Frame() {
  return std::string(R"({
  "log": {"level": "info"},
  "dns": {"servers": [
    {"type": "https", "tag": "remote", "server": "1.1.1.1", "detour": "proxy"},
    {"type": "https", "tag": "fi-dns", "server": "8.8.8.8", "detour": "FI"},
    {"type": "local", "tag": "local"}], "final": "remote"},
  "inbounds": [{"type": "tun", "tag": "tun-in", "address": ["172.19.0.1/30"], "auto_route": true}],
  "outbounds": [
    {"type": "selector", "tag": "proxy", "outbounds": ["auto", "NL", "FI"], "default": "FI"},
    {"type": "urltest", "tag": "auto", "outbounds": ["NL", "FI"]},
    {"type": "vless", "tag": "NL", "server": "nl.example.com", "server_port": 443,
     "uuid": "11111111-2222-3333-4444-555555555555", "flow": "xtls-rprx-vision",
     "tls": {"enabled": true, "server_name": "www.microsoft.com", "utls": {"enabled": true, "fingerprint": "chrome"},
             "reality": {"enabled": true, "public_key": ")") +
         std::string(kKey) + R"(", "short_id": "6ba8"}}},
    {"type": "trojan", "tag": "FI", "server": "fi.example.com", "server_port": 443, "password": "p",
     "tls": {"enabled": true}, "transport": {"type": "ws", "path": "/t"}},
    {"type": "direct", "tag": "direct"}],
  "route": {
    "rules": [{"action": "sniff"}, {"protocol": "dns", "action": "hijack-dns"},
              {"domain_suffix": ["example.fi"], "outbound": "FI"},
              {"rule_set": "ads", "outbound": "direct"}],
    "rule_set": [{"type": "remote", "tag": "ads", "format": "binary",
                  "url": "https://example.com/ads.srs", "download_detour": "FI"}],
    "final": "proxy", "auto_detect_interface": true, "default_domain_resolver": "local"}
})";
}

// Keys pasted by hand: our own config around them - one named NL, too.
std::string Keys() {
  const auto built = BuildConfigFromLinks({"trojan://pw@nl2.example.com:443#NL", "trojan://pw@de.example.com:443#DE",
                                           "ss://YWVzLTEyOC1nY206cA@1.2.3.4:8388#SS"});
  return built.config.value_or("{}");
}

json Parsed(const CombinedConfig& combined) {
  if (!combined.config) {
    std::cerr << "  no config: " << combined.error << "\n";
    return json::object();
  }
  return json::parse(*combined.config);
}

json Tagged(const json& config, std::string_view tag) {
  for (const char* list : {"outbounds", "endpoints"}) {
    for (const json& o : config.value(list, json::array())) {
      if (o.value("tag", "") == tag) {
        return o;
      }
    }
  }
  return json();
}

std::vector<std::string> Members(const json& config, std::string_view tag) {
  std::vector<std::string> members;
  const json group = Tagged(config, tag);
  if (group.is_object()) {
    for (const json& m : group.value("outbounds", json::array())) {
      members.push_back(m.get<std::string>());
    }
  }
  return members;
}

std::vector<std::pair<std::string, std::string>>& Checked() {
  static std::vector<std::pair<std::string, std::string>> checked;
  return checked;
}

void TestAlone() {
  const std::string frame = Frame();
  CHECK(CombineConfigs({{"A", frame, {}}}).config == std::optional<std::string>(frame));  // untouched
  CHECK(!CombineConfigs({}).config);
}

void TestServerOff() {
  const auto combined = CombineConfigs({{"A", Frame(), {"FI"}}});
  const json c = Parsed(combined);
  Checked().emplace_back("server-off", combined.config.value_or("{}"));
  CHECK(Tagged(c, "FI").is_null());
  CHECK(Members(c, "proxy") == std::vector<std::string>({"auto", "NL"}));
  CHECK(Members(c, "auto") == std::vector<std::string>({"NL"}));
  CHECK(!Tagged(c, "proxy").contains("default"));  // it was FI
  // What went through FI goes through the selector now.
  CHECK(c["route"]["rules"][2]["outbound"] == "proxy");
  CHECK(c["route"]["rules"][3]["outbound"] == "direct");
  CHECK(c["dns"]["servers"][1]["detour"] == "proxy");
  CHECK(c["route"]["rule_set"][0]["download_detour"] == "proxy");

  // Every server off: nothing to run, and it says so.
  const auto none = CombineConfigs({{"A", Frame(), {"FI", "NL"}}});
  CHECK(!none.config && !none.error.empty());
}

void TestChain() {
  ordered_json frame = ordered_json::parse(Frame());
  frame["outbounds"].push_back({{"type", "shadowtls"}, {"tag", "stls"}, {"server", "s.example.com"},
                                {"server_port", 443}, {"version", 3}, {"password", "p"},
                                {"tls", {{"enabled", true}, {"server_name", "www.microsoft.com"}}}});
  frame["outbounds"].push_back({{"type", "shadowsocks"}, {"tag", "SS-TLS"}, {"method", "2022-blake3-aes-128-gcm"},
                                {"password", "AAAAAAAAAAAAAAAAAAAAAA=="}, {"detour", "stls"}});
  frame["outbounds"][0]["outbounds"].push_back("SS-TLS");
  const auto combined = CombineConfigs({{"A", frame.dump(), {"stls"}}});
  const json c = Parsed(combined);
  CHECK(Tagged(c, "SS-TLS").is_null());  // chained through the one switched off
  CHECK(Members(c, "proxy") == std::vector<std::string>({"auto", "NL", "FI"}));

  const auto labels = ListServers(frame.dump());
  CHECK(labels.size() == 4);
  if (labels.size() == 4) {
    CHECK(labels[0].tag == "NL" && labels[0].label == "VLESS · REALITY");
    CHECK(labels[1].label == "Trojan · WS");
    CHECK(labels[2].label == "ShadowTLS");
    CHECK(labels[3].label == "Shadowsocks · через stls");
  }
  CHECK(ListServers("not json").empty());
}

void TestTwo() {
  const auto combined = CombineConfigs({{"A", Frame(), {}}, {"Ключи", Keys(), {"SS"}}});
  const json c = Parsed(combined);
  Checked().emplace_back("two", combined.config.value_or("{}"));
  CHECK(combined.notes.empty());
  // The frame's own outbounds first, then the others' servers; a clash renamed.
  CHECK(!Tagged(c, "NL · Ключи").is_null());
  CHECK(Tagged(c, "NL · Ключи")["server"] == "nl2.example.com");
  CHECK(!Tagged(c, "DE").is_null());
  CHECK(Tagged(c, "SS").is_null());  // switched off in its configuration
  CHECK(Members(c, "proxy") == std::vector<std::string>({"auto", "NL", "FI", "NL · Ключи", "DE"}));
  CHECK(Members(c, "auto") == std::vector<std::string>({"NL", "FI", "NL · Ключи", "DE"}));
  // Not their groups, their DNS or their routing: the frame's.
  CHECK(Tagged(c, "auto 2").is_null());
  std::size_t selectors = 0;
  for (const json& o : c["outbounds"]) {
    selectors += o["type"] == "selector" ? 1 : 0;
  }
  CHECK(selectors == 1);
  CHECK(c["route"]["final"] == "proxy");
  CHECK(c["dns"]["servers"].size() == 3);

  // The order decides the frame: the keys' config first.
  const json swapped = Parsed(CombineConfigs({{"Ключи", Keys(), {}}, {"A", Frame(), {}}}));
  CHECK(swapped["route"]["final"] == "proxy");
  CHECK(!Tagged(swapped, "NL · A").is_null());
  CHECK(Tagged(swapped, "stls").is_null());
}

void TestNoSelector() {
  // A hand-written config: one server and the route straight to it.
  const std::string single = R"({"dns":{"servers":[{"type":"local","tag":"own-dns"}]},)"
                             R"("inbounds":[{"type":"mixed","tag":"in","listen_port":2080}],)"
                             R"("outbounds":[{"type":"trojan","tag":"mine","server":"m.example.com","server_port":443,)"
                             R"("password":"p","tls":{"enabled":true},"domain_resolver":"own-dns"},)"
                             R"({"type":"direct","tag":"direct"}],"route":{"final":"mine"}})";
  const auto combined = CombineConfigs({{"Свой", single, {}}, {"Ключи", Keys(), {}}});
  const json c = Parsed(combined);
  Checked().emplace_back("no-selector", combined.config.value_or("{}"));
  CHECK(c["route"]["final"] == "proxy");
  CHECK(Tagged(c, "proxy")["default"] == "mine");
  CHECK(Members(c, "proxy") == std::vector<std::string>({"mine", "NL", "DE", "SS"}));

  // A server of another part that uses a DNS server the frame doesn't have:
  // the frame's resolver instead.
  const auto reversed = CombineConfigs({{"Ключи", Keys(), {}}, {"Свой", single, {}}});
  const json r = Parsed(reversed);
  Checked().emplace_back("resolver", reversed.config.value_or("{}"));
  CHECK(!Tagged(r, "mine").is_null() && !Tagged(r, "mine").contains("domain_resolver"));

  // A part that isn't JSON is left out, and said so.
  const auto broken = CombineConfigs({{"A", Frame(), {}}, {"B", "{oops", {}}});
  CHECK(broken.config.has_value() && broken.notes.size() == 1);
}

int CheckWithSingBox(const std::string& singBox) {
  const auto dir = std::filesystem::temp_directory_path() / "sovereign-combine-check";
  std::filesystem::create_directories(dir);
  int failed = 0;
  for (const auto& [name, config] : Checked()) {
    const auto path = dir / (name + ".json");
    std::ofstream(path, std::ios::binary) << config;
    std::wstring command = L"\"" + std::filesystem::path(singBox).wstring() + L"\" check -c \"" + path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    DWORD code = 1;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
      WaitForSingleObject(process.hProcess, INFINITE);
      GetExitCodeProcess(process.hProcess, &code);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    }
    std::cout << (code == 0 ? "ok     " : "FAILED ") << name << "\n";
    failed += code == 0 ? 0 : 1;
  }
  return failed;
}

}  // namespace

// `--try <sing-box.exe> <config.json>`: a real config as the frame, with the
// keys added, and with each of its servers switched off in turn - all through
// sing-box's check. Prints only names and results, not the config.
int TryReal(const std::string& singBox, const std::string& path) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  const std::string real((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto add = [](const std::string& name, const CombinedConfig& combined) {
    if (!combined.config) {
      std::cout << "none   " << name << ": " << combined.error << "\n";
      return;
    }
    Checked().emplace_back(name, *combined.config);
  };
  add("real+keys", CombineConfigs({{"real", real, {}}, {"Ключи", Keys(), {}}}));
  add("keys+real", CombineConfigs({{"Ключи", Keys(), {}}, {"real", real, {}}}));
  int n = 0;
  for (const ServerInfo& server : ListServers(real)) {
    add(std::format("real-off-{}", ++n), CombineConfigs({{"real", real, {server.tag}}}));
  }
  return CheckWithSingBox(singBox);
}

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  if (argc == 4 && std::string_view(argv[1]) == "--try") {
    return TryReal(argv[2], argv[3]);
  }
  try {
    TestAlone();
    TestServerOff();
    TestChain();
    TestTwo();
    TestNoSelector();
    if (argc == 3 && std::string_view(argv[1]) == "--check" && CheckWithSingBox(argv[2]) != 0) {
      return 1;
    }
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
