// sovereign-tray: the user's side of Sovereign. A notification-area icon that
// turns the proxy on and off through the service's control pipe and shows
// state and speed. Decisions live in TrayModel (tray_model.h, unit-tested);
// this file is Win32 glue: a message-only window on the UI thread and a worker
// thread that talks to the service once a second.
//
// Quitting the tray doesn't stop the box - the service runs it, the tray only
// steers. The next tray start picks the saved on/off intent up again.

#include <windows.h>
#include <shellapi.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "icons.h"
#include "pipe_client.h"
#include "settings.h"
#include "tray_model.h"

namespace {

using sovereign::tray::Action;
using sovereign::tray::Display;
using sovereign::tray::TrayModel;

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kViewChangedMessage = WM_APP + 2;
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuToggle = 1;
constexpr UINT kMenuOpenFolder = 2;
constexpr UINT kMenuExit = 3;

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n);
  return wide;
}

std::wstring FormatRate(double bytesPerSecond) {
  if (bytesPerSecond < 1024) {
    return std::format(L"{:.0f} Б/с", bytesPerSecond);
  }
  if (bytesPerSecond < 1024 * 1024) {
    return std::format(L"{:.1f} КБ/с", bytesPerSecond / 1024);
  }
  return std::format(L"{:.1f} МБ/с", bytesPerSecond / (1024 * 1024));
}

// What the UI thread draws; the worker publishes a fresh copy after each poll.
struct View {
  Display display = Display::ServiceDown;
  bool wantOn = false;
  double down = 0;
  double up = 0;
  std::int64_t connections = 0;
  std::string error;
};

std::wstring StatusLine(const View& v) {
  switch (v.display) {
    case Display::ServiceDown: return L"служба Sovereign не запущена";
    case Display::Off: return L"выключен";
    case Display::Starting: return L"подключается…";
    case Display::On:
      return std::format(L"вкл · ↓ {} ↑ {} · соединений: {}", FormatRate(v.down), FormatRate(v.up), v.connections);
    case Display::Error: return L"ошибка: " + Widen(v.error);
  }
  return {};
}

// State shared between the UI thread and the worker.
struct Shared {
  std::mutex mutex;
  std::condition_variable_any wake;
  std::optional<bool> pendingWant;  // a toggle the worker hasn't applied yet
  View view;
  HWND window = nullptr;
};

Shared& State() {
  static Shared shared;
  return shared;
}

// "" on success, otherwise why not - from the service's error response or ours.
std::string ServiceCall(const nlohmann::json& request, const char* successCmd) {
  const auto response = sovereign::tray::RequestService(request.dump());
  if (!response) {
    return "служба не ответила";
  }
  const auto json = nlohmann::json::parse(*response, nullptr, /*allow_exceptions=*/false);
  if (json.is_object() && json.value("cmd", "") == successCmd) {
    return {};
  }
  if (json.is_object() && json.contains("message") && json["message"].is_string()) {
    return json["message"].get<std::string>();
  }
  return "непонятный ответ службы";
}

std::string StartBox() {
  const auto config = sovereign::tray::LoadConfig();
  if (!config) {
    return "нет config.json в %LOCALAPPDATA%\\Sovereign";
  }
  auto parsed = nlohmann::json::parse(*config, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return "config.json: не JSON";
  }
  nlohmann::json request;
  request["cmd"] = "box_start";
  request["config"] = std::move(parsed);
  return ServiceCall(request, "box_started");
}

std::string StopBox() { return ServiceCall(nlohmann::json{{"cmd", "box_stop"}}, "box_stopped"); }

std::optional<sovereign::tray::Stats> PollStats() {
  const auto response = sovereign::tray::RequestService(R"({"cmd":"box_stats"})");
  if (!response) {
    return std::nullopt;
  }
  const auto json = nlohmann::json::parse(*response, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object() || json.value("cmd", "") != "box_stats") {
    // "gocore not loaded" and the like: the service is up, the box can't run.
    return sovereign::tray::Stats{};
  }
  return sovereign::tray::Stats{
      .running = json.value("running", false),
      .uplinkBytes = json.value("uplink", std::int64_t{0}),
      .downlinkBytes = json.value("downlink", std::int64_t{0}),
      .connections = json.value("connections", std::int64_t{0}),
      .generation = json.value("generation", std::int64_t{0}),
  };
}

void Execute(TrayModel& model, Action action) {
  if (action == Action::Start) {
    model.OnStartResult(StartBox(), TrayModel::Clock::now());
  } else if (action == Action::Stop) {
    model.OnStopResult(StopBox());
  }
}

void Worker(const std::stop_token& stop, bool wantOn) {
  auto& shared = State();
  TrayModel model(wantOn);
  while (!stop.stop_requested()) {
    std::optional<bool> want;
    {
      const std::scoped_lock lock(shared.mutex);
      want = std::exchange(shared.pendingWant, std::nullopt);
    }
    if (want) {
      Execute(model, model.SetWantOn(*want, TrayModel::Clock::now()));
    }
    Execute(model, model.OnPoll(PollStats(), TrayModel::Clock::now()));

    HWND window = nullptr;
    {
      const std::scoped_lock lock(shared.mutex);
      shared.view = View{model.GetDisplay(), model.WantOn(), model.DownRate(), model.UpRate(),
                         model.Connections(), model.LastError()};
      window = shared.window;
    }
    PostMessageW(window, kViewChangedMessage, 0, 0);

    std::unique_lock lock(shared.mutex);
    shared.wake.wait_for(lock, stop, std::chrono::seconds(1), [&] { return shared.pendingWant.has_value(); });
  }
}

// NOTIFYICONDATA as RAII: the icon leaves the notification area whatever
// happens to the window.
class TrayIcon {
 public:
  explicit TrayIcon(HWND window) {
    data_.cbSize = sizeof(NOTIFYICONDATAW);
    data_.hWnd = window;
    data_.uID = kTrayIconId;
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data_.uCallbackMessage = kTrayCallbackMessage;
    Update(View{});
    Add();
  }
  ~TrayIcon() { Shell_NotifyIconW(NIM_DELETE, &data_); }
  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;
  TrayIcon(TrayIcon&&) = delete;
  TrayIcon& operator=(TrayIcon&&) = delete;

  // Explorer restarted: the notification area is new and empty.
  void Add() { Shell_NotifyIconW(NIM_ADD, &data_); }

  void Update(const View& view) {
    auto& icon = icons_[static_cast<std::size_t>(view.display)];
    if (!icon) {
      icon = sovereign::tray::MakeStateIcon(view.display);
    }
    data_.hIcon = icon.get();
    wcsncpy_s(data_.szTip, (L"Sovereign — " + StatusLine(view)).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data_);
  }

 private:
  NOTIFYICONDATAW data_{};
  std::array<wil::unique_hicon, 5> icons_;
};

void RequestToggle() {
  auto& shared = State();
  bool want = false;
  {
    const std::scoped_lock lock(shared.mutex);
    want = !(shared.pendingWant ? *shared.pendingWant : shared.view.wantOn);
    shared.pendingWant = want;
  }
  try {
    sovereign::tray::SaveSettings({.wantOn = want});
  } catch (...) {
    // The toggle still applies for this session; WIL has reported the cause.
    LOG_CAUGHT_EXCEPTION_MSG("saving tray.json failed");
  }
  shared.wake.notify_all();
}

void ShowMenu(HWND window) {
  View view;
  {
    const std::scoped_lock lock(State().mutex);
    view = State().view;
  }
  wil::unique_hmenu menu(CreatePopupMenu());
  if (!menu) {
    return;
  }
  AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, StatusLine(view).c_str());
  AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu.get(), MF_STRING, kMenuToggle, view.wantOn ? L"Выключить" : L"Включить");
  AppendMenuW(menu.get(), MF_STRING, kMenuOpenFolder, L"Открыть папку настроек");
  AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu.get(), MF_STRING, kMenuExit, L"Выход (соединение остаётся)");

  POINT cursor{};
  GetCursorPos(&cursor);
  // Without the foreground switch the menu doesn't close on an outside click
  // (documented TrackPopupMenu behavior for notification icons).
  SetForegroundWindow(window);
  const auto command = static_cast<UINT>(TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x,
                                                         cursor.y, 0, window, nullptr));
  PostMessageW(window, WM_NULL, 0, 0);

  switch (command) {
    case kMenuToggle:
      RequestToggle();
      break;
    case kMenuOpenFolder:
      try {
        ShellExecuteW(nullptr, L"open", sovereign::tray::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      } catch (...) {
        LOG_CAUGHT_EXCEPTION();
      }
      break;
    case kMenuExit:
      DestroyWindow(window);
      break;
    default:
      break;
  }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  static TrayIcon* trayIcon = nullptr;
  static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  if (message == taskbarCreated && trayIcon != nullptr) {
    trayIcon->Add();
    return 0;
  }
  switch (message) {
    case WM_CREATE: {
      static TrayIcon icon(window);
      trayIcon = &icon;
      return 0;
    }
    case kViewChangedMessage:
      if (trayIcon != nullptr) {
        View view;
        {
          const std::scoped_lock lock(State().mutex);
          view = State().view;
        }
        trayIcon->Update(view);
      }
      return 0;
    case kTrayCallbackMessage:
      if (lParam == WM_LBUTTONUP) {
        RequestToggle();
      } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
        ShowMenu(window);
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

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
  // One tray per session: a second one would fight the first over the box.
  const wil::unique_mutex_nothrow single(CreateMutexW(nullptr, FALSE, L"Local\\SovereignTray"));
  if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
    return 0;
  }

  bool wantOn = false;
  try {
    wantOn = sovereign::tray::LoadSettings().wantOn;
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("reading tray settings failed; starting turned off");
  }

  const wchar_t kClassName[] = L"SovereignTrayWindow";
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = kClassName;
  if (!RegisterClassW(&windowClass)) {
    return 1;
  }
  // A message-only window can't receive broadcasts like TaskbarCreated, so
  // this one is a normal (never shown) top-level window.
  HWND window = CreateWindowExW(0, kClassName, L"sovereign tray", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (!window) {
    return 1;
  }
  {
    const std::scoped_lock lock(State().mutex);
    State().window = window;
    State().view.wantOn = wantOn;
  }

  int exitCode = 0;
  {
    std::jthread worker(Worker, wantOn);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    exitCode = static_cast<int>(msg.wParam);
    worker.request_stop();
  }  // joins the worker before the window's state goes away
  return exitCode;
}
