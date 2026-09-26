// Per-app routing rules (src/tray/app_rules.h).

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "app_rules.h"
#include "check.h"

namespace {

using nlohmann::json;
using sovereign::tray::AppsMode;
using sovereign::tray::ApplyAppRules;

// The shape packetlab's subscription has (sub/packetlab-sub.py).
constexpr std::string_view kSubscription = R"({
  "outbounds": [{"type":"selector","tag":"proxy","outbounds":["a"]},{"type":"anytls","tag":"a"},{"type":"direct","tag":"direct"}],
  "route": {"rules": [{"action":"sniff"},{"protocol":"dns","action":"hijack-dns"},
                      {"ip_is_private":true,"outbound":"direct"},{"protocol":"quic","action":"reject"}],
            "final": "proxy"}
})";

json Apply(std::string_view config, AppsMode mode, const std::vector<std::string>& apps) {
  return json::parse(ApplyAppRules(config, mode, apps));
}

void TestExcludeGoesDirectAfterPrelude() {
  const json c = Apply(kSubscription, AppsMode::Exclude, {"steam.exe", "qbittorrent.exe"});
  const json& rules = c["route"]["rules"];
  CHECK(rules.size() == 5);
  CHECK(rules[0]["action"] == "sniff");
  CHECK(rules[1]["action"] == "hijack-dns");
  CHECK(rules[2]["process_name"] == json({"steam.exe", "qbittorrent.exe"}));
  CHECK(rules[2]["outbound"] == "direct");
  CHECK(rules[3].value("ip_is_private", false));
  CHECK(c["route"]["final"] == "proxy");  // everything else still proxied
}

void TestIncludeProxiesOnlyTheList() {
  const json c = Apply(kSubscription, AppsMode::Include, {"chrome.exe"});
  const json& rules = c["route"]["rules"];
  CHECK(rules[2]["process_name"] == json({"chrome.exe"}));
  CHECK(rules[2]["outbound"] == "proxy");  // where final pointed
  CHECK(c["route"]["final"] == "direct");
}

void TestEmptyListChangesNothing() {
  CHECK(Apply(kSubscription, AppsMode::Exclude, {}) == json::parse(kSubscription));
  CHECK(Apply(kSubscription, AppsMode::Include, {}) == json::parse(kSubscription));
}

void TestMissingPiecesAreAdded() {
  // No direct outbound, no route at all: include mode needs both.
  const json c = Apply(R"({"outbounds":[{"type":"vless","tag":"v"}]})", AppsMode::Include, {"x.exe"});
  CHECK(c["outbounds"].size() == 2);
  CHECK(c["outbounds"][1]["type"] == "direct" && c["outbounds"][1]["tag"] == "direct-apps");
  CHECK(c["route"]["rules"][0]["outbound"] == "v");  // no final: the first outbound
  CHECK(c["route"]["final"] == "direct-apps");
}

void TestNotJsonComesBackAsIs() {
  CHECK(ApplyAppRules("not json", AppsMode::Exclude, {"a.exe"}) == "not json");
  CHECK(ApplyAppRules("[1]", AppsMode::Exclude, {"a.exe"}) == "[1]");
}

void TestModeNames() {
  CHECK(sovereign::tray::ParseAppsMode("include") == AppsMode::Include);
  CHECK(sovereign::tray::ParseAppsMode("exclude") == AppsMode::Exclude);
  CHECK(sovereign::tray::ParseAppsMode("garbage") == AppsMode::Exclude);
  CHECK(sovereign::tray::AppsModeName(AppsMode::Include) == "include");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestExcludeGoesDirectAfterPrelude();
    TestIncludeProxiesOnlyTheList();
    TestEmptyListChangesNothing();
    TestMissingPiecesAreAdded();
    TestNotJsonComesBackAsIs();
    TestModeNames();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
