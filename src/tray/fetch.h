#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "subscription.h"

namespace sovereign::tray {

struct FetchResult {
  std::string body;
  std::optional<std::chrono::hours> updateInterval;  // Profile-Update-Interval, if sent
  std::optional<std::string> title;                  // Profile-Title, if sent (profiles.h)
  std::optional<std::string> supportUrl;             // Support-Url, if sent and a sane link
  std::optional<std::string> webPageUrl;             // Profile-Web-Page-Url, the same
  std::optional<SubscriptionUsage> usage;            // Subscription-Userinfo, if sent
};

// GET over https with WinHTTP (system proxy settings, 10-30 s timeouts, the
// body capped at kMaxSubscriptionBytes), with `hwid` as x-hwid when it is
// one (profiles.h IsHwid). The error is for the user and never contains the
// URL - it carries the subscription token. Blocking: call it off the UI thread.
std::expected<FetchResult, std::string> FetchSubscription(const std::wstring& url, const std::wstring& userAgent,
                                                          std::string_view hwid);

// The same through the subscription relay (relay.h): the subscription's server
// sees the relay's address (Cloudflare's), never the user's. A big answer is
// fetched piece by piece, a connection each. The errors never carry the URL
// or the key.
std::expected<FetchResult, std::string> FetchSubscriptionViaRelay(const std::string& relayUrl, const std::string& relayKey,
                                                                  const std::wstring& url, const std::wstring& userAgent,
                                                                  std::string_view hwid);

// Any other GET over https, same client, the body capped at maxBytes; an
// error for anything but a 200. Blocking: call it off the UI thread.
std::expected<std::string, std::string> Download(const std::wstring& url, const std::wstring& userAgent,
                                                 std::size_t maxBytes);

// Any request over https with `headers` ("Name: value", CRLF between),
// same client; the answer's body, an error for anything but a 200.
// Blocking: call it off the UI thread.
std::expected<std::string, std::string> HttpsSend(const std::wstring& url, const std::wstring& userAgent,
                                                  const wchar_t* method, std::string_view body,
                                                  const std::wstring& headers, std::size_t maxBytes);

// A POST of `body` over https, same client; an error for anything but a 200.
// Blocking: call it off the UI thread.
std::expected<void, std::string> Upload(const std::wstring& url, const std::wstring& userAgent, std::string_view body);

// A speed test's download, same client: a GET read through piece by piece,
// nothing kept; `onBytes` is told how many came so far after each piece and
// says whether to go on. The bytes that came; an error only if none did (a
// server cutting a long stream off, a 3 s stall, is the end of it). The
// upload is Upload()s timed to the server's answer instead: written pieces
// are counted before they leave. Blocking: off the UI thread.
std::expected<std::uint64_t, std::string> StreamBytes(const std::wstring& url, const std::wstring& userAgent,
                                                      const std::function<bool(std::uint64_t)>& onBytes);

}  // namespace sovereign::tray