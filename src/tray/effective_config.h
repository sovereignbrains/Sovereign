#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "app_rules.h"

// What the box runs: config.json with the tray's own additions applied - the
// protocol pick, the per-app rules, the log level and the cache file. One
// function rather than four calls in main.cpp so the conformance harness
// (tests/conformance/harness.cpp) hands the core exactly what the tray would.
// Unit-tested in tests/unit/effective_config_test.cpp.
namespace sovereign::tray {

struct ConfigAdditions {
  std::string protocol{};                 // protocol_choice.h; empty = the config's
  AppsMode appsMode = AppsMode::Exclude;  // app_rules.h
  std::vector<std::string> apps{};
  std::string logLevel{};   // log_level.h; empty = the config's
  std::string cacheFile{};  // cache_file.h, UTF-8; empty = none
};

// nlohmann's dump (what the service hashes); text that isn't a JSON object
// comes back unchanged.
std::string EffectiveConfig(std::string_view config, const ConfigAdditions& additions);

}  // namespace sovereign::tray
