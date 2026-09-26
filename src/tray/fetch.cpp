#include "fetch.h"

#include <windows.h>
#include <winhttp.h>

#include <wil/resource.h>

#include <format>
#include <string_view>

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

std::expected<FetchResult, std::string> FetchSubscription(const std::wstring& url, const std::wstring& userAgent) {
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
    return std::unexpected(Failure("не удалось подключиться к серверу подписки"));
  }
  const wil::unique_winhttp_hinternet request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                                 WINHTTP_FLAG_SECURE));
  if (!request) {
    return std::unexpected(Failure("запрос не создался"));
  }
  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.get(), nullptr)) {
    return std::unexpected(Failure("сервер подписки не ответил"));
  }

  DWORD status = 0;
  DWORD statusSize = sizeof status;
  if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
    return std::unexpected(Failure("нет кода ответа"));
  }
  if (status != 200) {
    return std::unexpected(status == 404 ? std::string("сервер не знает такую подписку (404) - ссылку сменили?")
                                         : std::format("сервер ответил {}", status));
  }

  FetchResult result;
  wchar_t header[64]{};
  DWORD headerSize = sizeof header;
  if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CUSTOM, L"Profile-Update-Interval", header, &headerSize,
                          WINHTTP_NO_HEADER_INDEX)) {
    result.updateInterval = ParseUpdateInterval(Narrow(std::wstring_view(header, headerSize / sizeof(wchar_t))));
  }

  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.get(), &available)) {
      return std::unexpected(Failure("обрыв при чтении ответа"));
    }
    if (available == 0) {
      break;
    }
    if (result.body.size() + available > kMaxSubscriptionBytes) {
      return std::unexpected("ответ больше 4 МБ - это не конфиг");
    }
    const std::size_t offset = result.body.size();
    result.body.resize(offset + available);
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), result.body.data() + offset, available, &read)) {
      return std::unexpected(Failure("обрыв при чтении ответа"));
    }
    result.body.resize(offset + read);
  }
  return result;
}

}  // namespace sovereign::tray
