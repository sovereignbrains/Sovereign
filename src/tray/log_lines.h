#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sovereign::tray {

// A line of the core's log as the tray's log window shows it.
struct LogLine {
  std::int64_t timeMs = 0;  // Unix time in milliseconds; 0 = unknown
  std::string level;        // "error", "warn", "info"...
  std::string message;      // UTF-8, without sing-box's own level prefix
};

// One box_logs response: the lines, and where the next request starts.
struct LogsPage {
  std::vector<LogLine> lines;
  std::uint64_t next = 0;
  bool more = false;  // the service had more than fit: ask again right away
};

// nullopt if `response` isn't a box_logs answer (an error, or garbage).
std::optional<LogsPage> ParseLogsResponse(std::string_view response);

// sing-box starts every line with its level and the seconds since the box
// started - "ERROR[0012] outbound/..." - which the log window shows in columns
// of its own. Anything else is returned as is.
std::string_view StripCoreLevelPrefix(std::string_view message);

}  // namespace sovereign::tray
