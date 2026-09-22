#include <windows.h>
#include <shellapi.h>

#include <wil/resource.h>

#include <nlohmann/json.hpp>
#include <string>

#include "protocol.h"

namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;

// RAII для NOTIFYICONDATA: удаляет иконку из трея в деструкторе, что бы ни
// случилось с окном (WM_DESTROY, исключение, ранний return).
class TrayIcon {
 public:
  TrayIcon(HWND window, HICON icon, const wchar_t* tooltip) {
    data_.cbSize = sizeof(NOTIFYICONDATAW);
    data_.hWnd = window;
    data_.uID = kTrayIconId;
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data_.uCallbackMessage = kTrayCallbackMessage;
    data_.hIcon = icon;
    wcsncpy_s(data_.szTip, tooltip, _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &data_);
  }

  ~TrayIcon() { Shell_NotifyIconW(NIM_DELETE, &data_); }

  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  void ShowBalloon(const std::wstring& title, const std::wstring& text) {
    data_.uFlags |= NIF_INFO;
    wcsncpy_s(data_.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data_.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data_);
  }

 private:
  NOTIFYICONDATAW data_{};
};

std::wstring PingCore() {
  wil::unique_hfile pipe(CreateFileW(
      sovereign::ipc::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
      OPEN_EXISTING, 0, nullptr));
  if (!pipe) {
    return L"служба недоступна (pipe не открылся)";
  }

  nlohmann::json request;
  request["cmd"] = "ping";
  const std::string payload = request.dump();

  DWORD written = 0;
  if (!WriteFile(pipe.get(), payload.data(),
                  static_cast<DWORD>(payload.size()), &written, nullptr)) {
    return L"ошибка записи в pipe";
  }

  char buffer[sovereign::ipc::kPipeBufferSize]{};
  DWORD bytesRead = 0;
  if (!ReadFile(pipe.get(), buffer, sizeof(buffer), &bytesRead, nullptr)) {
    return L"ошибка чтения из pipe";
  }

  const std::string response(buffer, bytesRead);
  return L"ответ службы: " + std::wstring(response.begin(), response.end());
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                             LPARAM lParam) {
  static TrayIcon* trayIcon = nullptr;

  switch (message) {
    case WM_CREATE: {
      static TrayIcon icon(window, LoadIconW(nullptr, IDI_APPLICATION),
                            L"sovereign (P0 skeleton)");
      trayIcon = &icon;
      return 0;
    }
    case kTrayCallbackMessage:
      if (lParam == WM_LBUTTONUP && trayIcon != nullptr) {
        trayIcon->ShowBalloon(L"sovereign", PingCore());
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wParam, lParam);
  }
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR,
                       _In_ int) {
  const wchar_t kClassName[] = L"SovereignTrayWindow";

  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = kClassName;
  if (!RegisterClassW(&windowClass)) {
    return 1;
  }

  HWND window = CreateWindowExW(0, kClassName, L"sovereign tray", 0, 0, 0, 0,
                                 0, HWND_MESSAGE, nullptr, instance, nullptr);
  if (!window) {
    return 1;
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
