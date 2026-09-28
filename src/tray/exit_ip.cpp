#include "exit_ip.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>

#include "json_field.h"

namespace sovereign::tray {

namespace {

constexpr std::size_t kMaxAddress = 45;  // the longest IPv6 text form
constexpr std::size_t kMaxError = 300;

bool LooksLikeAddress(const std::string& ip) {
  return !ip.empty() && ip.size() <= kMaxAddress && std::all_of(ip.begin(), ip.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':';
  });
}

bool LooksLikeCountry(const std::string& code) {
  return code.size() == 2 && code[0] >= 'A' && code[0] <= 'Z' && code[1] >= 'A' && code[1] <= 'Z';
}

}  // namespace

std::optional<ExitIp> ParseExitIpResponse(std::string_view response) {
  const auto json = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);
  if (Field<std::string>(json, "cmd", {}) != "box_exitip") {
    return std::nullopt;
  }
  ExitIp exit;
  if (std::string ip = Field<std::string>(json, "ip", {}); LooksLikeAddress(ip)) {
    exit.ip = std::move(ip);
  }
  if (std::string country = Field<std::string>(json, "country", {}); LooksLikeCountry(country)) {
    exit.country = std::move(country);
  }
  exit.pending = Field<bool>(json, "pending", false);
  exit.error = Field<std::string>(json, "error", {}).substr(0, kMaxError);
  return exit;
}

}  // namespace sovereign::tray
