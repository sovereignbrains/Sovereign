#include "protocol_choice.h"

#include "json_field.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace sovereign::tray {

namespace {

using Json = nlohmann::json;

bool IsSelector(const Json& o) { return Field<std::string>(o, "type", {}) == "selector"; }

// The selector to use: the one route.final names, else the first one.
Json* FindSelector(Json& config) {
  if (!config.is_object() || !config.contains("outbounds") || !config["outbounds"].is_array()) {
    return nullptr;
  }
  Json& outbounds = config["outbounds"];
  std::string final;
  if (config.contains("route") && config["route"].is_object() && config["route"].contains("final") &&
      config["route"]["final"].is_string()) {
    final = config["route"]["final"].get<std::string>();
  }
  Json* first = nullptr;
  for (Json& o : outbounds) {
    if (!IsSelector(o)) {
      continue;
    }
    if (!final.empty() && Field<std::string>(o, "tag", {}) == final) {
      return &o;
    }
    if (first == nullptr) {
      first = &o;
    }
  }
  return first;
}

}  // namespace

ProtocolChoices FindProtocolChoices(std::string_view config) {
  Json json = Json::parse(config, nullptr, /*allow_exceptions=*/false);
  const Json* selector = FindSelector(json);
  ProtocolChoices choices;
  if (selector == nullptr) {
    return choices;
  }
  choices.selector = Field<std::string>(*selector, "tag", {});
  if (selector->contains("outbounds") && (*selector)["outbounds"].is_array()) {
    for (const Json& o : (*selector)["outbounds"]) {
      if (o.is_string()) {
        choices.options.push_back(o.get<std::string>());
      }
    }
  }
  choices.configDefault = Field<std::string>(*selector, "default", choices.options.empty() ? std::string{} : choices.options[0]);
  const auto isOption = [&](const std::string& tag) {
    return std::find(choices.options.begin(), choices.options.end(), tag) != choices.options.end();
  };
  for (const Json& o : json["outbounds"]) {
    const std::string tag = Field<std::string>(o, "tag", {});
    if (Field<std::string>(o, "type", {}) != "urltest" || !isOption(tag) || !o.contains("outbounds") ||
        !o["outbounds"].is_array()) {
      continue;
    }
    std::vector<std::string>& servers = choices.groups[tag];
    for (const Json& server : o["outbounds"]) {
      if (server.is_string() && isOption(server.get<std::string>())) {
        servers.push_back(server.get<std::string>());
      }
    }
  }
  return choices;
}

std::string ApplyProtocolChoice(std::string_view config, const std::string& choice) {
  Json json = Json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    return std::string(config);
  }
  Json* selector = FindSelector(json);
  if (selector != nullptr && !choice.empty() && selector->contains("outbounds") && (*selector)["outbounds"].is_array()) {
    const Json& options = (*selector)["outbounds"];
    if (std::find(options.begin(), options.end(), Json(choice)) != options.end()) {
      (*selector)["default"] = choice;
    }
  }
  return json.dump();
}

}  // namespace sovereign::tray
