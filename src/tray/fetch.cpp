#include "fetch.h"

#include <windows.h>
#include <winhttp.h>

#include <wil/resource.h>

#include <format>
#include <optional>
#include <string_view>
#include <utility>

#include "subscription.h"

namespace sovereign::tray {

namespace {

std::string Failure(std::string_view what) {
  return std::format("{} (код {})", what, GetLastError());
}

std::string Narrow(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace

namespace {

struct Response {
  DWORD status = 0;
  std::string body;
  std::optional<std::string> header;  // `headerName`'s value, if asked for and sent
};

// One GET over https; `what` names the server in errors ("сервер подписки").
// Redirects are followed https to https only (WinHTTP's default policy).
std::expected<Response, std::string> HttpsGet(const std::wstring& url, const std::wstring& userAgent,
                                              std::size_t maxBytes, const wchar_t* headerName, std::string_view what) {
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
  const wil::unique_winhttp_hinternet request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                                 WINHTTP_FLAG_SECURE));
  if (!request) {
    return std::unexpected(Failure("запрос не создался"));
  }
  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
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
  if (headerName != nullptr) {
    wchar_t header[64]{};
    DWORD headerSize = sizeof header;
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CUSTOM, headerName, header, &headerSize,
                            WINHTTP_NO_HEADER_INDEX)) {
      response.header = Narrow(std::wstring_view(header, headerSize / sizeof(wchar_t)));
    }
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
  auto response = HttpsGet(url, userAgent, kMaxSubscriptionBytes, L"Profile-Update-Interval", "сервер подписки");
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
  if (response->header) {
    result.updateInterval = ParseUpdateInterval(*response->header);
  }
  return result;
}

std::expected<std::string, std::string> Download(const std::wstring& url, const std::wstring& userAgent,
                                                 std::size_t maxBytes) {
  auto response = HttpsGet(url, userAgent, maxBytes, nullptr, "сервер");
  if (!response) {
    return std::unexpected(response.error());
  }
  if (response->status != 200) {
    return std::unexpected(std::format("сервер ответил {}", response->status));
  }
  return std::move(response->body);
}

}  // namespace sovereign::tray
