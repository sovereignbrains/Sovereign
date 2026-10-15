#include "log_lines.h"

#include "json_field.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstddef>
#include <utility>

namespace sovereign::tray {

std::optional<LogsPage> ParseLogsResponse(std::string_view response) {
  const auto json = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object() || Field<std::string>(json, "cmd", {}) != "box_logs" || !json.contains("entries") ||
      !json["entries"].is_array() || !json.contains("next") || !json["next"].is_number_unsigned()) {
    return std::nullopt;
  }
  LogsPage page;
  page.next = json["next"].get<std::uint64_t>();
  page.more = Field<bool>(json, "more", false);
  for (const auto& entry : json["entries"]) {
    if (!entry.is_object()) {
      continue;
    }
    LogLine line;
    line.timeMs = Field<std::int64_t>(entry, "time", 0);
    line.level = Field<std::string>(entry, "level", {});
    const std::string message = Field<std::string>(entry, "message", {});
    line.message = std::string(StripCoreLevelPrefix(message));
    if (Harmless(line.message)) {
      line.level = "debug";
    }
    page.lines.push_back(std::move(line));
  }
  return page;
}

std::string_view StripCoreLevelPrefix(std::string_view message) {
  std::size_t i = 0;
  while (i < message.size() && message[i] >= 'A' && message[i] <= 'Z') {
    ++i;
  }
  if (i == 0 || i >= message.size() || message[i] != '[') {
    return message;
  }
  std::size_t j = i + 1;
  while (j < message.size() && message[j] >= '0' && message[j] <= '9') {
    ++j;
  }
  if (j == i + 1 || j >= message.size() || message[j] != ']') {
    return message;
  }
  ++j;
  if (j < message.size() && message[j] == ' ') {
    ++j;
  }
  return message.substr(j);
}

bool Harmless(std::string_view message) {
  // A QUIC stream (Hysteria 2, TUIC) the other end closed with code 0 - no
  // error: a site done with a connection. sing-box says ERROR every time
  // (every 15 s with Telegram, 15.10.2026), which only scared.
  for (const std::string_view said : {"canceled by remote with error code 0", "canceled by local with error code 0"}) {
    if (const std::size_t at = message.find(said); at != std::string_view::npos) {
      const std::size_t end = at + said.size();
      return end == message.size() || std::isalnum(static_cast<unsigned char>(message[end])) == 0;  // not 0x10, 01
    }
  }
  return false;
}

}  // namespace sovereign::tray
