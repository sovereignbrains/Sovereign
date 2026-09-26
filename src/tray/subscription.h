#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The subscription's pure rules, apart from WinHTTP (fetch.h) so they can be
// unit-tested (tests/unit/subscription_test.cpp).

namespace sovereign::tray {

// Anything larger isn't a config - and couldn't cross the control pipe anyway
// (sovereign::ipc::kMaxMessageBytes).
inline constexpr std::size_t kMaxSubscriptionBytes = 4 * 1024 * 1024;

// How often to refresh when the server doesn't say (the header below).
inline constexpr std::chrono::hours kDefaultUpdateInterval{12};

// Only https: the URL carries the user's token and the answer their keys.
bool IsHttpsUrl(std::wstring_view url);

// A subscription answer the tray will hand to box_start: a JSON object with a
// non-empty "outbounds" array. On success, how many outbounds (proxies and the
// rest) it has; otherwise why not, for the user.
struct ConfigCheck {
  bool ok = false;
  std::size_t outbounds = 0;
  std::string error;
};
ConfigCheck CheckSubscriptionConfig(std::string_view body);

// Profile-Update-Interval (hours), the de-facto header subscription servers
// send (packetlab's does). nullopt if absent or not a sane number; clamped
// to 1 h .. 7 days.
std::optional<std::chrono::hours> ParseUpdateInterval(std::string_view header);

// Whether the config brings up a TUN inbound - which can't coexist with
// another sing-box's TUN on the same machine.
bool ConfigHasTun(std::string_view config);

}  // namespace sovereign::tray
