#include "subscription.h"

#include "json_field.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <string>
#include <vector>

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

std::wstring UrlHost(std::wstring_view url) {
  const auto scheme = url.find(L"://");
  if (scheme == std::wstring_view::npos) {
    return {};
  }
  std::wstring_view authority = url.substr(scheme + 3);
  authority = authority.substr(0, authority.find_first_of(L"/?#"));
  if (const auto at = authority.rfind(L'@'); at != std::wstring_view::npos) {
    authority.remove_prefix(at + 1);
  }
  return std::wstring(authority);
}

ConfigCheck CheckSubscriptionConfig(std::string_view body) {
  if (body.size() > kMaxSubscriptionBytes) {
    return {.error = "ответ больше 4 МБ — это не конфиг"};
  }
  const auto json = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (json.is_discarded()) {
    return {.error = "ответ не JSON (ссылка отдаёт не конфиг sing-box?)"};
  }
  if (!json.is_object()) {
    return {.error = "ответ — не объект конфига sing-box"};
  }
  const auto outbounds = json.find("outbounds");
  if (outbounds == json.end() || !outbounds->is_array() || outbounds->empty()) {
    return {.error = "в конфиге нет outbounds"};
  }
  return {.ok = true, .outbounds = outbounds->size(), .error = {}};
}

std::string ProviderNotice(std::string_view config) {
  const auto json = nlohmann::json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object() || !json.contains("outbounds") || !json["outbounds"].is_array()) {
    return {};
  }
  std::vector<std::string> servers;
  for (const auto& o : json["outbounds"]) {
    const std::string type = Field<std::string>(o, "type", {});
    if (!type.empty() && type != "selector" && type != "urltest" && type != "direct" && type != "block" &&
        type != "dns") {
      servers.push_back(Field<std::string>(o, "tag", {}));
    }
  }
  if (servers.size() != 1) {
    return {};
  }
  // Panels' stubs (08.10.2026, SPACE VPN: "🔴 Приложение не поддерживает
  // HWID"): a warning sign, or the words such messages are made of.
  static constexpr std::array<std::string_view, 16> kMarks = {
      "\xF0\x9F\x94\xB4" /* 🔴 */, "\xE2\x9A\xA0" /* ⚠ */,      "\xE2\x9D\x8C" /* ❌ */, "\xE2\x9B\x94" /* ⛔ */,
      "\xF0\x9F\x9A\xAB" /* 🚫 */, "HWID",                       "hwid",                  "не поддерж",
      "лимит",                     "истек",                      "истёк",                 "обновите",
      "продлите",                  "оплатите",                   "заблокир",              "expired"};
  const std::string& name = servers.front();
  const bool notice = std::any_of(kMarks.begin(), kMarks.end(),
                                  [&](std::string_view mark) { return name.find(mark) != std::string::npos; });
  return notice ? name : std::string();
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

std::optional<SubscriptionUsage> ParseSubscriptionUserinfo(std::string_view header) {
  SubscriptionUsage usage;
  bool any = false;
  while (!header.empty()) {
    const std::size_t semi = header.find(';');
    std::string_view part = header.substr(0, semi);
    header = semi == std::string_view::npos ? std::string_view() : header.substr(semi + 1);
    while (!part.empty() && part.front() == ' ') {
      part.remove_prefix(1);
    }
    while (!part.empty() && part.back() == ' ') {
      part.remove_suffix(1);
    }
    const std::size_t eq = part.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string_view key = part.substr(0, eq);
    const std::string_view value = part.substr(eq + 1);
    // Some panels send a float ("1.073741824e+10"): the integer part is enough.
    std::uint64_t n = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (ec != std::errc{} || end == value.data()) {
      continue;
    }
    if (key == "upload") {
      usage.upload = n;
    } else if (key == "download") {
      usage.download = n;
    } else if (key == "total") {
      usage.total = n;
    } else if (key == "expire") {
      usage.expire = static_cast<std::int64_t>(std::min<std::uint64_t>(n, std::uint64_t{1} << 40U));
    } else {
      continue;
    }
    any = true;
  }
  return any ? std::optional(usage) : std::nullopt;
}

std::optional<std::string> ParseLinkHeader(std::string_view header) {
  while (!header.empty() && header.front() == ' ') {
    header.remove_prefix(1);
  }
  while (!header.empty() && header.back() == ' ') {
    header.remove_suffix(1);
  }
  if (header.empty() || header.size() > kMaxLinkHeader) {
    return std::nullopt;
  }
  if (!header.starts_with("https://") && !header.starts_with("http://") && !header.starts_with("tg://")) {
    return std::nullopt;
  }
  if (std::any_of(header.begin(), header.end(), [](char c) { return c <= ' ' || c > '~'; })) {
    return std::nullopt;
  }
  return std::string(header);
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
    return Field<std::string>(inbound, "type", {}) == "tun";
  });
}

}  // namespace sovereign::tray
