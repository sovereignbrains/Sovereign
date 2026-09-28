#pragma once

#include <string>
#include <string_view>

// How much the core writes to the log: the config's log.level, overridden by
// the tray when the user picks one in the log page - applied to the config it
// sends, like the protocol pick, so the pick restarts the box. Unit-tested in
// tests/unit/log_level_test.cpp.

namespace sovereign::tray {

// sing-box's levels the tray offers, most to least verbose.
inline constexpr std::string_view kLogLevels[] = {"debug", "info", "warn", "error"};

bool IsLogLevel(std::string_view level);

// `config` with log.level = `level` and the log enabled (a config's own
// "disabled": true would leave the page empty). Unchanged but re-dumped if
// `level` is empty or unknown; text that isn't a JSON object comes back as
// it was.
std::string ApplyLogLevel(std::string_view config, std::string_view level);

}  // namespace sovereign::tray
