#include "log_level.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>

namespace sovereign::tray {

bool IsLogLevel(std::string_view level) {
  return std::find(std::begin(kLogLevels), std::end(kLogLevels), level) != std::end(kLogLevels);
}

std::string ApplyLogLevel(std::string_view config, std::string_view level) {
  nlohmann::json json = nlohmann::json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    return std::string(config);
  }
  if (IsLogLevel(level)) {
    nlohmann::json& log = json["log"];
    if (!log.is_object()) {
      log = nlohmann::json::object();
    }
    log["level"] = std::string(level);
    log["disabled"] = false;
  }
  return json.dump();
}

}  // namespace sovereign::tray
