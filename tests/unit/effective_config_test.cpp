// The config the tray sends to box_start (src/tray/effective_config.h).

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "effective_config.h"

namespace {

using nlohmann::json;
using sovereign::tray::AppsMode;
using sovereign::tray::ConfigAdditions;
using sovereign::tray::EffectiveConfig;

constexpr const char* kConfig = R"({
  "log": {"level": "info"},
  "outbounds": [
    {"type": "selector", "tag": "proxy", "outbounds": ["a", "b"]},
    {"type": "vless", "tag": "a", "server": "1.2.3.4", "server_port": 443},
    {"type": "anytls", "tag": "b", "server": "1.2.3.4", "server_port": 8443},
    {"type": "direct", "tag": "direct"}
  ],
  "route": {"rules": [{"action": "sniff"}], "final": "proxy"}
})";

void TestEveryAdditionApplied() {
  const json c = json::parse(EffectiveConfig(kConfig, ConfigAdditions{.protocol = "b",
                                                                      .appsMode = AppsMode::Exclude,
                                                                      .apps = {"game.exe"},
                                                                      .logLevel = "warn",
                                                                      .cacheFile = R"(C:\cache.db)"}));
  CHECK(c["outbounds"][0]["default"] == "b");
  CHECK(c["route"]["rules"][0]["action"] == "sniff");  // the app rule goes after it
  CHECK(c["route"]["rules"][1]["process_name"] == json::array({"game.exe"}));
  CHECK(c["route"]["rules"][1]["outbound"] == "direct");
  CHECK(c["log"]["level"] == "warn");
  CHECK(c["experimental"]["cache_file"]["path"] == R"(C:\cache.db)");
  // The outbounds the box dials are left as they were.
  CHECK(c["outbounds"][1] == json::parse(kConfig)["outbounds"][1]);
  CHECK(c["outbounds"][2] == json::parse(kConfig)["outbounds"][2]);
}

void TestNoAdditions() {
  CHECK(json::parse(EffectiveConfig(kConfig, ConfigAdditions{})) == json::parse(kConfig));
  CHECK(EffectiveConfig("not json", ConfigAdditions{.cacheFile = "x"}) == "not json");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestEveryAdditionApplied();
    TestNoAdditions();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
