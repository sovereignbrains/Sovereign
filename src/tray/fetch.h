#pragma once

#include <chrono>
#include <expected>
#include <optional>
#include <string>

namespace sovereign::tray {

struct FetchResult {
  std::string body;
  std::optional<std::chrono::hours> updateInterval;  // Profile-Update-Interval, if sent
};

// GET over https with WinHTTP (system proxy settings, 10-30 s timeouts, the
// body capped at kMaxSubscriptionBytes). The error is for the user and never
// contains the URL - it carries the subscription token. Blocking: call it off
// the UI thread.
std::expected<FetchResult, std::string> FetchSubscription(const std::wstring& url, const std::wstring& userAgent);

}  // namespace sovereign::tray
