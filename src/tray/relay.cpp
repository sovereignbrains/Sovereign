#include "relay.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <format>

#include "profiles.h"

namespace sovereign::tray {

namespace {

std::optional<std::size_t> Number(std::string_view text) {
  std::size_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// 8-4-4-4-12 lowercase hex: crypto.randomUUID()'s shape.
bool IsUuid(std::string_view text) {
  if (text.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? ch != '-' : !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool IsRelayUrl(std::string_view url) {
  constexpr std::string_view kScheme = "https://";
  if (!url.starts_with(kScheme) || url.size() > 2048) {
    return false;
  }
  const std::string_view rest = url.substr(kScheme.size());
  const std::string_view host = rest.substr(0, rest.find('/'));
  const bool bad = std::any_of(url.begin(), url.end(),
                               [](char ch) { return ch == '?' || ch == '#' || ch == ' ' || ch < 0x21 || ch > 0x7E; });
  return !host.empty() && host.find('@') == std::string_view::npos && !bad;
}

bool IsRelayKey(std::string_view key) {
  return key.size() >= 16 && key.size() <= 256 &&
         std::all_of(key.begin(), key.end(), [](char ch) { return ch > 0x20 && ch < 0x7F; });
}

std::string RelayBase(std::string_view url) {
  std::string base(url);
  while (base.ends_with('/')) {
    base.pop_back();
  }
  return base + "/";
}

std::string RelayRequestBody(std::string_view url, std::string_view userAgent, std::string_view hwid) {
  nlohmann::json headers = {{"User-Agent", std::string(userAgent)}};
  if (IsHwid(hwid)) {
    headers["x-hwid"] = std::string(hwid);
  }
  return nlohmann::json{{"url", std::string(url)}, {"headers", std::move(headers)}}.dump();
}

std::optional<RelayPlan> ParseRelayPlan(std::string_view job, std::string_view size, std::string_view chunk,
                                        std::size_t maxBytes) {
  const auto bytes = Number(size);
  const auto piece = Number(chunk);
  if (!IsUuid(job) || !bytes || !piece || *bytes == 0 || *piece == 0 || *bytes > maxBytes) {
    return std::nullopt;
  }
  RelayPlan plan{.job = std::string(job), .size = *bytes, .chunk = *piece, .count = (*bytes + *piece - 1) / *piece};
  if (plan.count > kMaxRelayPieces) {
    return std::nullopt;
  }
  return plan;
}

std::string RelayChunkPath(const RelayPlan& plan, std::size_t n) { return std::format("chunk?job={}&n={}", plan.job, n); }

}  // namespace sovereign::tray
