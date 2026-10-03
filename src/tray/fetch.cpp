#include "fetch.h"

#include <windows.h>
#include <winhttp.h>

#include <wil/resource.h>

#include <array>
#include <format>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "profiles.h"
#include "relay.h"
#include "subscription.h"

namespace sovereign::tray {

namespace {

std::string Failure(std::string_view what) {
  return std::format("{} (код {})", what, GetLastError());
}

}  // namespace

namespace {

struct Response {
  DWORD status = 0;
  std::string body;
  std::vector<std::optional<std::string>> headers;  // per name asked for: its value, if sent
};

// One GET over https; `what` names the server in errors ("сервер подписки").
// Redirects are followed https to https only (WinHTTP's default policy).
std::expected<Response, std::string> HttpsGet(const std::wstring& url, const std::wstring& userAgent,
                                              std::size_t maxBytes, std::initializer_list<const wchar_t*> headerNames,
                                              std::string_view what, const wchar_t* method = L"GET",
                                              std::string_view body = {}, const std::wstring& extraHeaders = {}) {
  if (!IsHttpsUrl(url)) {
    return std::unexpected("ссылка должна быть https://");
  }

  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof parts;
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
    return std::unexpected("ссылка не разбирается как https-адрес");
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

  const wil::unique_winhttp_hinternet session(
      WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    return std::unexpected(Failure("WinHTTP не открылся"));
  }
  WinHttpSetTimeouts(session.get(), 10'000, 10'000, 15'000, 30'000);

  const wil::unique_winhttp_hinternet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
  if (!connection) {
    return std::unexpected(Failure(std::format("не удалось подключиться: {}", what)));
  }
  const wil::unique_winhttp_hinternet request(WinHttpOpenRequest(connection.get(), method, path.c_str(), nullptr,
                                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                                 WINHTTP_FLAG_SECURE));
  if (!request) {
    return std::unexpected(Failure("запрос не создался"));
  }
  // WinHTTP takes the body as non-const; it doesn't write to it.
  auto* data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
  if (!WinHttpSendRequest(request.get(), extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str(),
                          extraHeaders.empty() ? 0 : static_cast<DWORD>(-1), data, static_cast<DWORD>(body.size()),
                          static_cast<DWORD>(body.size()), 0) ||
      !WinHttpReceiveResponse(request.get(), nullptr)) {
    return std::unexpected(Failure(std::format("{} не ответил", what)));
  }

  Response response;
  DWORD statusSize = sizeof response.status;
  if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
    return std::unexpected(Failure("нет кода ответа"));
  }
  for (const wchar_t* name : headerNames) {
    // WinHTTP widens the header's bytes one by one (ISO-8859-1): narrowed
    // back the same way, UTF-8 in it (Profile-Title) comes out whole.
    std::array<wchar_t, 1024> header{};
    DWORD headerSize = static_cast<DWORD>(header.size() * sizeof(wchar_t));
    std::optional<std::string> value;
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CUSTOM, name, header.data(), &headerSize,
                            WINHTTP_NO_HEADER_INDEX)) {
      std::string bytes;
      for (std::size_t i = 0; i < headerSize / sizeof(wchar_t); ++i) {
        bytes.push_back(static_cast<char>(header[i] & 0xFF));
      }
      value = std::move(bytes);
    }
    response.headers.push_back(std::move(value));
  }
  if (response.status != 200) {
    return response;
  }

  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.get(), &available)) {
      return std::unexpected(Failure("обрыв при чтении ответа"));
    }
    if (available == 0) {
      break;
    }
    if (response.body.size() + available > maxBytes) {
      return std::unexpected(std::format("ответ больше {} МБ", maxBytes / (std::size_t{1024} * 1024)));
    }
    const std::size_t offset = response.body.size();
    response.body.resize(offset + available);
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), response.body.data() + offset, available, &read)) {
      return std::unexpected(Failure("обрыв при чтении ответа"));
    }
    response.body.resize(offset + read);
  }
  return response;
}

}  // namespace

namespace {

std::string SubscriptionStatusError(DWORD status) {
  return status == 404 ? std::string("сервер не знает такую подписку (404) — ссылку сменили?")
                       : std::format("сервер ответил {}", status);
}

// What a subscription answer's headers say about it: [interval, title] at `at`.
void ReadSubscriptionHeaders(const Response& response, std::size_t at, FetchResult& result) {
  if (response.headers.size() >= at + 2) {
    if (response.headers[at]) {
      result.updateInterval = ParseUpdateInterval(*response.headers[at]);
    }
    if (response.headers[at + 1]) {
      result.title = ParseProfileTitle(*response.headers[at + 1]);
    }
  }
}

std::string Utf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring Wide(std::string_view text) { return {text.begin(), text.end()}; }  // ASCII: relay URLs, keys, paths

}  // namespace

std::expected<FetchResult, std::string> FetchSubscription(const std::wstring& url, const std::wstring& userAgent,
                                                          std::string_view hwid) {
  // Only x-hwid: no OS version, no device model - nothing of the machine's.
  const std::wstring headers = IsHwid(hwid) ? L"x-hwid: " + std::wstring(hwid.begin(), hwid.end()) : std::wstring();
  auto response = HttpsGet(url, userAgent, kMaxSubscriptionBytes, {L"Profile-Update-Interval", L"Profile-Title"},
                           "сервер подписки", L"GET", {}, headers);
  if (!response) {
    return std::unexpected(response.error());
  }
  if (response->status != 200) {
    return std::unexpected(SubscriptionStatusError(response->status));
  }
  FetchResult result;
  result.body = std::move(response->body);
  ReadSubscriptionHeaders(*response, 0, result);
  return result;
}

std::expected<FetchResult, std::string> FetchSubscriptionViaRelay(const std::string& relayUrl, const std::string& relayKey,
                                                                  const std::wstring& url, const std::wstring& userAgent,
                                                                  std::string_view hwid) {
  if (!IsRelayUrl(relayUrl) || !IsRelayKey(relayKey)) {
    return std::unexpected(std::string("пересыльщик настроен неверно (Настройки)"));
  }
  if (!IsHttpsUrl(url)) {
    return std::unexpected(std::string("ссылка должна быть https://"));
  }
  const std::string base = RelayBase(relayUrl);
  const std::wstring key = L"X-Relay-Key: " + Wide(relayKey);
  auto response = HttpsGet(Wide(base), userAgent, kMaxSubscriptionBytes,
                           {L"X-Relay-Status", L"X-Relay-Job", L"X-Relay-Size", L"X-Relay-Chunk", L"Profile-Update-Interval",
                            L"Profile-Title"},
                           "пересыльщик", L"POST", RelayRequestBody(Utf8(url), Utf8(userAgent), hwid),
                           key + L"\r\nContent-Type: application/json");
  if (!response) {
    return std::unexpected(response.error());
  }
  const auto header = [&](std::size_t i) -> std::string_view {
    return response->headers.size() > i && response->headers[i] ? std::string_view(*response->headers[i]) : std::string_view();
  };
  // The subscription server's own answer carries X-Relay-Status; without it
  // the relay itself refused (a wrong key or address: a bare 404).
  if (header(0).empty()) {
    return std::unexpected(response->status == 404
                               ? std::string("пересыльщик не принял запрос — проверь адрес и ключ (Настройки)")
                               : std::format("пересыльщик не смог скачать подписку ({})", response->status));
  }
  if (response->status != 200) {
    return std::unexpected(SubscriptionStatusError(response->status));
  }
  FetchResult result;
  ReadSubscriptionHeaders(*response, 4, result);
  if (header(1).empty()) {
    result.body = std::move(response->body);
    return result;
  }
  const auto plan = ParseRelayPlan(header(1), header(2), header(3), kMaxSubscriptionBytes);
  if (!plan) {
    return std::unexpected(std::string("пересыльщик ответил непонятно"));
  }
  // Each piece over a session - a connection - of its own (HttpsGet opens one).
  result.body.reserve(plan->size);
  for (std::size_t n = 0; n < plan->count; ++n) {
    auto piece = HttpsGet(Wide(base + RelayChunkPath(*plan, n)), userAgent, plan->chunk, {}, "пересыльщик", L"GET", {}, key);
    if (!piece) {
      return std::unexpected(piece.error());
    }
    if (piece->status != 200) {
      return std::unexpected(std::format("пересыльщик не отдал часть {} из {} ({})", n + 1, plan->count, piece->status));
    }
    result.body += piece->body;
  }
  if (result.body.size() != plan->size) {
    return std::unexpected(std::string("подписка пришла не целиком"));
  }
  return result;
}

std::expected<std::string, std::string> Download(const std::wstring& url, const std::wstring& userAgent,
                                                 std::size_t maxBytes) {
  auto response = HttpsGet(url, userAgent, maxBytes, {}, "сервер");
  if (!response) {
    return std::unexpected(response.error());
  }
  if (response->status != 200) {
    return std::unexpected(std::format("сервер ответил {}", response->status));
  }
  return std::move(response->body);
}

std::expected<std::string, std::string> HttpsSend(const std::wstring& url, const std::wstring& userAgent,
                                                  const wchar_t* method, std::string_view body,
                                                  const std::wstring& headers, std::size_t maxBytes) {
  auto response = HttpsGet(url, userAgent, maxBytes, {}, "сервер", method, body, headers);
  if (!response) {
    return std::unexpected(response.error());
  }
  if (response->status != 200) {
    return std::unexpected(std::format("сервер ответил {}", response->status));
  }
  return std::move(response->body);
}

std::expected<void, std::string> Upload(const std::wstring& url, const std::wstring& userAgent, std::string_view body) {
  auto response = HttpsGet(url, userAgent, std::size_t{1024} * 1024, {}, "сервер", L"POST", body);
  if (!response) {
    return std::unexpected(response.error());
  }
  if (response->status != 200) {
    return std::unexpected(std::format("сервер ответил {}", response->status));
  }
  return {};
}

}  // namespace sovereign::tray