#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
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

// The host (and port) of a URL, for showing where the subscription comes from:
// the rest - path, query, user info - is where the token lives. Empty if
// there is no "scheme://host".
std::wstring UrlHost(std::wstring_view url);

// A subscription answer the tray will hand to box_start: a JSON object with a
// non-empty "outbounds" array. On success, how many outbounds (proxies and the
// rest) it has; otherwise why not, for the user.
struct ConfigCheck {
  bool ok = false;
  std::size_t outbounds = 0;
  std::string error;
};
ConfigCheck CheckSubscriptionConfig(std::string_view body);

// A panel's message dressed as a server - the subscription's one server named
// "🔴 Приложение не поддерживает HWID", "лимит устройств" and the like: its
// name, for the user; empty for a real subscription. Taking it would replace
// the working servers with one that connects nowhere.
std::string ProviderNotice(std::string_view config);

// Profile-Update-Interval (hours), the de-facto header subscription servers
// send (packetlab's does). nullopt if absent or not a sane number; clamped
// to 1 h .. 7 days.
std::optional<std::chrono::hours> ParseUpdateInterval(std::string_view header);

// Subscription-Userinfo (Marzban, Remnawave, 3x-ui...): "upload=N; download=N;
// total=N; expire=UNIX" - bytes, and the end of the paid period. total and
// expire 0 (or absent): no limit, no end. nullopt if there's nothing usable.
struct SubscriptionUsage {
  std::uint64_t upload = 0;
  std::uint64_t download = 0;
  std::uint64_t total = 0;  // 0: no limit
  std::int64_t expire = 0;  // unix seconds; 0: no end
};
std::optional<SubscriptionUsage> ParseSubscriptionUserinfo(std::string_view header);

// Support-Url / Profile-Web-Page-Url: a link to open for the user - https://,
// http:// or tg://, printable ASCII, at most kMaxLinkHeader bytes. Anything
// else (a script: link, a control character) is nullopt.
inline constexpr std::size_t kMaxLinkHeader = 512;
std::optional<std::string> ParseLinkHeader(std::string_view header);

// Whether the config brings up a TUN inbound - which can't coexist with
// another sing-box's TUN on the same machine.
bool ConfigHasTun(std::string_view config);

}  // namespace sovereign::tray
