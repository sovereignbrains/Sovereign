#include "app_rules.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace sovereign::tray {

namespace {

using Json = nlohmann::json;

// The tag of the config's direct outbound, adding one if there is none.
std::string DirectTag(Json& config) {
  Json& outbounds = config["outbounds"];
  if (!outbounds.is_array()) {
    outbounds = Json::array();
  }
  for (const Json& o : outbounds) {
    if (o.is_object() && o.value("type", "") == "direct" && o.contains("tag") && o["tag"].is_string()) {
      return o["tag"].get<std::string>();
    }
  }
  outbounds.push_back({{"type", "direct"}, {"tag", "direct-apps"}});
  return "direct-apps";
}

// Where proxied traffic goes: route.final, else the first outbound (sing-box's
// own default).
std::string ProxyTag(const Json& config) {
  const Json& route = config["route"];
  if (route.contains("final") && route["final"].is_string()) {
    return route["final"].get<std::string>();
  }
  for (const Json& o : config["outbounds"]) {
    if (o.is_object() && o.contains("tag") && o["tag"].is_string()) {
      return o["tag"].get<std::string>();
    }
  }
  return {};
}

}  // namespace

std::string_view AppsModeName(AppsMode mode) { return mode == AppsMode::Include ? "include" : "exclude"; }

AppsMode ParseAppsMode(std::string_view name) { return name == "include" ? AppsMode::Include : AppsMode::Exclude; }

std::string ApplyAppRules(std::string_view config, AppsMode mode, const std::vector<std::string>& apps) {
  Json json = Json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    return std::string(config);
  }
  if (apps.empty()) {
    return json.dump();
  }
  if (!json["route"].is_object()) {
    json["route"] = Json::object();
  }
  const std::string direct = DirectTag(json);
  const std::string proxy = ProxyTag(json);

  Json& rules = json["route"]["rules"];
  if (!rules.is_array()) {
    rules = Json::array();
  }
  const auto isPrelude = [](const Json& r) {
    return r.is_object() && (r.value("action", "") == "sniff" || r.value("action", "") == "hijack-dns");
  };
  const auto at = std::find_if_not(rules.begin(), rules.end(), isPrelude);

  Json rule;
  rule["process_name"] = apps;
  if (mode == AppsMode::Exclude) {
    rule["outbound"] = direct;
  } else {
    rule["outbound"] = proxy;
    json["route"]["final"] = direct;
  }
  rules.insert(at, std::move(rule));
  return json.dump();
}

}  // namespace sovereign::tray
