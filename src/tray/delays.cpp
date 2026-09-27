#include "delays.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

#include "json_field.h"

namespace sovereign::tray {

std::optional<std::map<std::string, Delay>> ParseDelaysResponse(std::string_view response) {
  const auto json = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);
  if (Field<std::string>(json, "cmd", {}) != "box_delays" || !json.contains("results") ||
      !json["results"].is_array()) {
    return std::nullopt;
  }
  std::map<std::string, Delay> delays;
  for (const auto& item : json["results"]) {
    const std::string tag = Field<std::string>(item, "tag", {});
    if (tag.empty()) {
      continue;
    }
    Delay delay;
    const std::int64_t ms = Field<std::int64_t>(item, "delay", 0);
    const std::string error = Field<std::string>(item, "error", {});
    if (ms > 0) {
      delay.state = Delay::State::Ok;
      delay.ms = static_cast<int>(std::min<std::int64_t>(ms, 65535));
    } else if (!error.empty()) {
      delay.state = Delay::State::Failed;
      delay.error = error;
    } else if (!Field<bool>(item, "pending", false)) {
      continue;  // not a result
    }
    delays[tag] = std::move(delay);
  }
  return delays;
}

}  // namespace sovereign::tray
