#include "cache_file.h"

#include <nlohmann/json.hpp>

namespace sovereign::tray {

std::string ApplyCacheFile(std::string_view config, const std::string& path) {
  nlohmann::json json = nlohmann::json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    return std::string(config);
  }
  nlohmann::json& experimental = json["experimental"];
  if (!experimental.is_object()) {
    experimental = nlohmann::json::object();
  }
  nlohmann::json& cache = experimental["cache_file"];
  if (!cache.is_object()) {
    cache = nlohmann::json::object();
  }
  cache["enabled"] = true;
  cache["path"] = path;
  return json.dump();
}

}  // namespace sovereign::tray
