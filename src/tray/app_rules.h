#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Per-app routing: which programs go through the proxy. Applied by the tray to
// the config it sends (config.json itself stays as the subscription delivered
// it), matched by sing-box's process_name - the exe name, not the path, which
// changes with every update for apps installed per version (app-1.12.3\...).
// Unit-tested in tests/unit/app_rules_test.cpp.

namespace sovereign::tray {

enum class AppsMode : std::uint8_t {
  Exclude,  // everything through the proxy except the listed apps (default)
  Include,  // only the listed apps through the proxy, the rest direct
};

std::string_view AppsModeName(AppsMode mode);             // "exclude" / "include"
AppsMode ParseAppsMode(std::string_view name);            // unknown -> Exclude

// The config to send: `config` with one rule for `apps` inserted right after
// the leading sniff/hijack-dns rules (those must still see every connection),
// before anything that routes by domain or IP:
//   Exclude: {"process_name": apps, "outbound": <direct>}
//   Include: {"process_name": apps, "outbound": <route.final or the first
//            outbound>}, and route.final becomes <direct>.
// <direct> is the config's direct outbound, added if it has none. An empty
// list changes nothing. The result is nlohmann's dump (what the service
// hashes); text that isn't a JSON object comes back unchanged.
std::string ApplyAppRules(std::string_view config, AppsMode mode, const std::vector<std::string>& apps);

}  // namespace sovereign::tray
