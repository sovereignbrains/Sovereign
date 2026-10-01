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
                                              std::string_view body = {}) {
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
  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, static_cast<DWORD>(body.size()),
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
  if (response.status != 200) {
    return response;
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

std::expected<FetchResult, std::string> FetchSubscription(const std::wstring& url, const std::wstring& userAgent) {
  auto response = HttpsGet(url, userAgent, kMaxSubscriptionBytes, {L"Profile-Update-Interval", L"Profile-Title"},
                           "сервер подписки");
  if (!response) {
    return std::unexpected(response.error());
  }
  if (response->status != 200) {
    return std::unexpected(response->status == 404
                               ? std::string("сервер не знает такую подписку (404) - ссылку сменили?")
                               : std::format("сервер ответил {}", response->status));
  }
  FetchResult result;
  result.body = std::move(response->body);
  if (response->headers.size() == 2) {
    if (response->headers[0]) {
      result.updateInterval = ParseUpdateInterval(*response->headers[0]);
    }
    if (response->headers[1]) {
      result.title = ParseProfileTitle(*response->headers[1]);
    }
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