#include "subscription.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cwctype>

namespace sovereign::tray {

bool IsHttpsUrl(std::wstring_view url) {
  constexpr std::wstring_view kScheme = L"https://";
  if (url.size() <= kScheme.size()) {
    return false;
  }
  for (std::size_t i = 0; i < kScheme.size(); ++i) {
    if (std::towlower(url[i]) != kScheme[i]) {
      return false;
    }
  }
  // A host must follow, and nothing that could split the request line.
  return url[kScheme.size()] != L'/' &&
         std::none_of(url.begin(), url.end(), [](wchar_t c) { return c <= L' ' || c == 0x7F; });
}

ConfigCheck CheckSubscriptionConfig(std::string_view body) {
  if (body.size() > kMaxSubscriptionBytes) {
    return {.error = "ответ больше 4 МБ - это не конфиг"};
  }
  const auto json = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (json.is_discarded()) {
    return {.error = "ответ не JSON (ссылка отдаёт не конфиг sing-box?)"};
  }
  if (!json.is_object()) {
    return {.error = "ответ - не объект конфига sing-box"};
  }
  const auto outbounds = json.find("outbounds");
  if (outbounds == json.end() || !outbounds->is_array() || outbounds->empty()) {
    return {.error = "в конфиге нет outbounds"};
  }
  return {.ok = true, .outbounds = outbounds->size(), .error = {}};
}

std::optional<std::chrono::hours> ParseUpdateInterval(std::string_view header) {
  while (!header.empty() && header.front() == ' ') {
    header.remove_prefix(1);
  }
  while (!header.empty() && header.back() == ' ') {
    header.remove_suffix(1);
  }
  int hours = 0;
  const auto [end, ec] = std::from_chars(header.data(), header.data() + header.size(), hours);
  if (header.empty() || ec != std::errc{} || end != header.data() + header.size() || hours <= 0) {
    return std::nullopt;
  }
  return std::chrono::hours(std::clamp(hours, 1, 24 * 7));
}

bool ConfigHasTun(std::string_view config) {
  const auto json = nlohmann::json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    return false;
  }
  const auto inbounds = json.find("inbounds");
  if (inbounds == json.end() || !inbounds->is_array()) {
    return false;
  }
  return std::any_of(inbounds->begin(), inbounds->end(), [](const nlohmann::json& inbound) {
    return inbound.is_object() && inbound.value("type", "") == "tun";
  });
}

}  // namespace sovereign::tray
