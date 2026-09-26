#include "protocol_choice.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace sovereign::tray {

namespace {

using Json = nlohmann::json;

bool IsSelector(const Json& o) { return o.is_object() && o.value("type", "") == "selector"; }

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
    if (!final.empty() && o.value("tag", "") == final) {
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
  choices.selector = selector->value("tag", "");
  if (selector->contains("outbounds") && (*selector)["outbounds"].is_array()) {
    for (const Json& o : (*selector)["outbounds"]) {
      if (o.is_string()) {
        choices.options.push_back(o.get<std::string>());
      }
    }
  }
  choices.configDefault = selector->value("default", choices.options.empty() ? std::string{} : choices.options[0]);
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
