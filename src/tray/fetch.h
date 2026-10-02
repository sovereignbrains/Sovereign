#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace sovereign::tray {

struct FetchResult {
  std::string body;
  std::optional<std::chrono::hours> updateInterval;  // Profile-Update-Interval, if sent
  std::optional<std::string> title;                  // Profile-Title, if sent (profiles.h)
};

// GET over https with WinHTTP (system proxy settings, 10-30 s timeouts, the
// body capped at kMaxSubscriptionBytes), with `hwid` as x-hwid when it is
// one (profiles.h IsHwid). The error is for the user and never contains the
// URL - it carries the subscription token. Blocking: call it off the UI thread.
std::expected<FetchResult, std::string> FetchSubscription(const std::wstring& url, const std::wstring& userAgent,
                                                          std::string_view hwid);

// Any other GET over https, same client, the body capped at maxBytes; an
// error for anything but a 200. Blocking: call it off the UI thread.
std::expected<std::string, std::string> Download(const std::wstring& url, const std::wstring& userAgent,
                                                 std::size_t maxBytes);

// A POST of `body` over https, same client; an error for anything but a 200.
// Blocking: call it off the UI thread.
std::expected<void, std::string> Upload(const std::wstring& url, const std::wstring& userAgent, std::string_view body);

}  // namespace sovereign::tray