// sovereign-tray: the user's side of Sovereign. A notification-area icon that
// turns the proxy on and off through the service's control pipe, shows state
// and speed, and keeps the config fresh from a subscription URL. Decisions
// live in TrayModel and subscription.h (both unit-tested); this file is Win32
// glue: a hidden window on the UI thread, and a worker thread that owns the
// settings and does everything that blocks (the pipe, WinHTTP) once a second
// or when the UI asks.
//
// Quitting the tray doesn't stop the box - the service runs it, the tray only
// steers. The next tray start picks the saved on/off intent up again.

// winsock2 first: WIN32_LEAN_AND_MEAN leaves it out, iphlpapi.h needs it.
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <wil/com.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "app_rules.h"
#include "fetch.h"
#include "protocol_choice.h"
#include "flyout.h"
#include "icons.h"
#include "pipe_client.h"
#include "settings.h"
#include "sha256.h"
#include "subscription.h"
#include "tray_model.h"

namespace {

using sovereign::tray::Action;
using sovereign::tray::AppsMode;
using sovereign::tray::Display;
using sovereign::tray::TraySettings;
using sovereign::tray::TrayModel;

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kViewChangedMessage = WM_APP + 2;
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuAddRunning = 400;  // + index in RunningApps()
constexpr std::size_t kMenuMaxItems = 250;

// What the subscription server sees: Sovereign runs the official sing-box core
// (no Mieru), so servers that pick a format by User-Agent - packetlab's does -
// give it the plain sing-box one. The version is the pinned core's.
constexpr wchar_t kUserAgent[] = L"Sovereign/0.1 (sing-box " SOVEREIGN_SINGBOX_VERSION_W L")";

// After a failed refresh, when to try again (the old config keeps working).
constexpr std::chrono::minutes kRefreshRetry{30};

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n);
  return wide;
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
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

std::wstring LocalTime(std::int64_t unixSeconds) {
  const auto t = static_cast<std::time_t>(unixSeconds);
  std::tm tm{};
  localtime_s(&tm, &t);
  wchar_t text[32]{};
  wcsftime(text, 32, L"%d.%m %H:%M", &tm);
  return text;
}

// What the UI thread draws; the worker publishes a fresh copy after each poll.
struct View {
  Display display = Display::ServiceDown;
  bool wantOn = false;
  double down = 0;
  double up = 0;
  std::int64_t connections = 0;
  std::string error;
  bool hasSubscription = false;
  std::int64_t lastRefresh = 0;
  std::string subscriptionError;  // the last refresh failed with this
  // A balloon to show once: the UI shows it when noticeId changes.
  unsigned noticeId = 0;
  std::wstring noticeTitle;
  std::wstring noticeText;
  bool noticeIsError = false;
  AppsMode appsMode = AppsMode::Exclude;
  std::vector<std::string> apps;
  std::vector<std::string> protocols;  // the config selector's options
  int protocol = -1;                   // the one in use
};

// A new per-app setup from the flyout.
struct AppsChange {
  AppsMode mode = AppsMode::Exclude;
  std::vector<std::string> list;
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

// Requests from the UI thread to the worker, and the view back.
struct Shared {
  std::mutex mutex;
  std::condition_variable_any wake;
  std::optional<bool> pendingWant;
  std::optional<std::string> pendingImport;  // a new subscription URL (UTF-8)
  bool pendingRefresh = false;
  std::optional<AppsChange> pendingApps;
  std::optional<std::string> pendingProtocol;  // a selector option; "" = the config's own default
  View view;
  HWND window = nullptr;

  bool HasRequests() const {
    return pendingWant || pendingImport || pendingRefresh || pendingApps || pendingProtocol;
  }
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

// An up sing-tun adapter that isn't ours: another sing-box client's TUN (only
// asked while our box isn't running, so it can't be ours). Two of them fight
// over the default route and the machine loses its internet.
std::optional<std::wstring> ForeignSingTun() {
  ULONG size = 16 * 1024;
  std::vector<std::byte> buffer;
  ULONG result = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
    buffer.resize(size);
    result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }
  if (result != NO_ERROR) {
    return std::nullopt;  // can't tell - let the start try
  }
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a != nullptr; a = a->Next) {
    if (a->OperStatus == IfOperStatusUp && a->Description != nullptr &&
        std::wstring_view(a->Description) == L"sing-tun Tunnel") {
      return std::wstring(a->FriendlyName != nullptr ? a->FriendlyName : L"?");
    }
  }
  return std::nullopt;
}

// `config` is the effective one: config.json with the per-app rules applied.
std::string StartBox(const std::optional<std::string>& config) {
  if (!config) {
    return "нет конфига: вставь ссылку-подписку (меню) или положи config.json в %LOCALAPPDATA%\\Sovereign";
  }
  auto parsed = nlohmann::json::parse(*config, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return "config.json: не JSON";
  }
  if (sovereign::tray::ConfigHasTun(*config)) {
    if (const auto other = ForeignSingTun()) {
      return "уже работает другой клиент sing-box с TUN (адаптер " + Narrow(*other) +
             ") - выключи его, Sovereign подключится сам";
    }
  }
  nlohmann::json request;
  request["cmd"] = "box_start";
  request["config"] = std::move(parsed);
  return ServiceCall(request, "box_started");
}

// The hash box_stats reports for the effective config, were it running: the
// service hashes the config as nlohmann dumps it (objects with sorted keys),
// so the same dump gives the same hash. Empty when there is no usable
// config.json - then nothing is enforced.
std::string ExpectedConfigHash(const std::optional<std::string>& config) {
  if (!config) {
    return {};
  }
  const auto parsed = nlohmann::json::parse(*config, nullptr, /*allow_exceptions=*/false);
  return parsed.is_discarded() ? std::string{} : sovereign::Sha256Hex(parsed.dump());
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
      .configSha256 = json.value("config_sha256", std::string{}),
  };
}

void Execute(TrayModel& model, Action action, const std::optional<std::string>& config) {
  if (action == Action::Start) {
    model.OnStartResult(StartBox(config), TrayModel::Clock::now());
  } else if (action == Action::Stop) {
    model.OnStopResult(StopBox());
  }
}

// The worker's state besides the model: the settings it owns and saves.
class Worker {
 public:
  explicit Worker(TraySettings settings) : settings_(std::move(settings)), model_(settings_.wantOn) {}

  void Run(const std::stop_token& stop) {
    auto& shared = State();
    while (!stop.stop_requested()) {
      std::optional<bool> want;
      std::optional<std::string> import;
      std::optional<AppsChange> apps;
      std::optional<std::string> protocol;
      bool refresh = false;
      {
        const std::scoped_lock lock(shared.mutex);
        want = std::exchange(shared.pendingWant, std::nullopt);
        apps = std::exchange(shared.pendingApps, std::nullopt);
        protocol = std::exchange(shared.pendingProtocol, std::nullopt);
        import = std::exchange(shared.pendingImport, std::nullopt);
        refresh = std::exchange(shared.pendingRefresh, false);
      }
      if (want) {
        settings_.wantOn = *want;
        Save();
        Execute(model_, model_.SetWantOn(*want, TrayModel::Clock::now()), EffectiveConfig());
      }
      if (import) {
        settings_.subscriptionUrl = *import;
        settings_.lastRefresh = 0;
        Save();
        Refresh();
      } else if (refresh || RefreshDue()) {
        Refresh();
      }
      if (apps) {
        settings_.appsMode = apps->mode;
        settings_.apps = std::move(apps->list);
        Save();  // the effective config changes: the model restarts the box
      }
      if (protocol) {
        settings_.protocol = *protocol;
        Save();  // likewise
      }
      const auto config = EffectiveConfig();
      model_.SetExpectedConfig(ExpectedConfigHash(config));
      Execute(model_, model_.OnPoll(PollStats(), TrayModel::Clock::now()), config);
      Publish();

      std::unique_lock lock(shared.mutex);
      shared.wake.wait_for(lock, stop, std::chrono::seconds(1), [&] { return shared.HasRequests(); });
    }
  }

 private:
  // config.json with the protocol pick and the per-app rules applied - what
  // the box must run.
  std::optional<std::string> EffectiveConfig() const {
    const auto config = sovereign::tray::LoadConfig();
    if (!config) {
      return std::nullopt;
    }
    return sovereign::tray::ApplyAppRules(sovereign::tray::ApplyProtocolChoice(*config, settings_.protocol),
                                          settings_.appsMode, settings_.apps);
  }

  // The selector's options and which one the box uses: the user's pick if
  // the config still has it, else the config's default.
  std::pair<std::vector<std::string>, int> Protocols() const {
    const auto config = sovereign::tray::LoadConfig();
    if (!config) {
      return {{}, -1};
    }
    const auto choices = sovereign::tray::FindProtocolChoices(*config);
    const auto find = [&](const std::string& tag) {
      const auto it = std::find(choices.options.begin(), choices.options.end(), tag);
      return it == choices.options.end() ? -1 : static_cast<int>(it - choices.options.begin());
    };
    int current = settings_.protocol.empty() ? -1 : find(settings_.protocol);
    if (current < 0) {
      current = find(choices.configDefault);
    }
    return {choices.options, current};
  }

  void Save() {
    try {
      sovereign::tray::SaveSettings(settings_);
    } catch (...) {
      // Still applies for this session; WIL has reported the cause.
      LOG_CAUGHT_EXCEPTION_MSG("saving tray.json failed");
    }
  }

  bool RefreshDue() const {
    if (settings_.subscriptionUrl.empty()) {
      return false;
    }
    if (lastFailure_ && TrayModel::Clock::now() - *lastFailure_ < kRefreshRetry) {
      return false;
    }
    return std::time(nullptr) - settings_.lastRefresh >= std::int64_t{settings_.updateHours} * 3600;
  }

  void Notify(std::wstring title, std::wstring text, bool isError) {
    ++noticeId_;
    noticeTitle_ = std::move(title);
    noticeText_ = std::move(text);
    noticeIsError_ = isError;
  }

  void Refresh() {
    if (settings_.subscriptionUrl.empty()) {
      return;
    }
    const auto fetched = sovereign::tray::FetchSubscription(Widen(settings_.subscriptionUrl), kUserAgent);
    std::string error;
    sovereign::tray::ConfigCheck check;
    if (!fetched) {
      error = fetched.error();
    } else {
      check = sovereign::tray::CheckSubscriptionConfig(fetched->body);
      error = check.error;
    }
    if (!error.empty()) {
      lastFailure_ = TrayModel::Clock::now();
      subscriptionError_ = error;
      Notify(L"Sovereign: подписка не обновилась", Widen(error) + L" (работает прежний конфиг)", true);
      return;
    }
    lastFailure_.reset();
    subscriptionError_.clear();

    const auto previous = sovereign::tray::LoadConfig();
    const bool changed = !previous || *previous != fetched->body;
    try {
      if (changed) {
        sovereign::tray::SaveConfig(fetched->body);
      }
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("saving config.json failed");
      subscriptionError_ = "не удалось сохранить config.json";
      return;
    }
    settings_.lastRefresh = std::time(nullptr);
    settings_.updateHours =
        static_cast<int>(fetched->updateInterval.value_or(sovereign::tray::kDefaultUpdateInterval).count());
    Save();
    Notify(L"Sovereign: подписка обновлена",
           std::format(L"выходов в конфиге: {}{}", check.outbounds, changed ? L"" : L" (без изменений)"), false);
    // A running box with the old config is restarted by the model: the hash
    // the service reports no longer matches (ExpectedConfigHash).
  }

  void Publish() {
    auto& shared = State();
    auto protocols = Protocols();  // reads config.json: outside the lock
    HWND window = nullptr;
    {
      const std::scoped_lock lock(shared.mutex);
      View& v = shared.view;
      v.display = model_.GetDisplay();
      v.wantOn = model_.WantOn();
      v.down = model_.DownRate();
      v.up = model_.UpRate();
      v.connections = model_.Connections();
      v.error = model_.LastError();
      v.hasSubscription = !settings_.subscriptionUrl.empty();
      v.lastRefresh = settings_.lastRefresh;
      v.subscriptionError = subscriptionError_;
      v.noticeId = noticeId_;
      v.noticeTitle = noticeTitle_;
      v.noticeText = noticeText_;
      v.noticeIsError = noticeIsError_;
      v.appsMode = settings_.appsMode;
      v.apps = settings_.apps;
      std::tie(v.protocols, v.protocol) = protocols;
      window = shared.window;
    }
    PostMessageW(window, kViewChangedMessage, 0, 0);
  }

  TraySettings settings_;
  TrayModel model_;
  std::optional<TrayModel::Clock::time_point> lastFailure_;
  std::string subscriptionError_;
  unsigned noticeId_ = 0;
  std::wstring noticeTitle_;
  std::wstring noticeText_;
  bool noticeIsError_ = false;
};

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
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    if (view.noticeId != shownNotice_) {
      shownNotice_ = view.noticeId;
      data_.uFlags |= NIF_INFO;
      data_.dwInfoFlags = view.noticeIsError ? NIIF_ERROR : NIIF_INFO;
      wcsncpy_s(data_.szInfoTitle, view.noticeTitle.c_str(), _TRUNCATE);
      wcsncpy_s(data_.szInfo, view.noticeText.c_str(), _TRUNCATE);
    }
    Shell_NotifyIconW(NIM_MODIFY, &data_);
  }

  // A balloon from the UI thread itself (e.g. nothing usable on the clipboard).
  void Balloon(const std::wstring& title, const std::wstring& text) {
    data_.uFlags = NIF_INFO;
    data_.dwInfoFlags = NIIF_WARNING;
    wcsncpy_s(data_.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data_.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data_);
  }

 private:
  NOTIFYICONDATAW data_{};
  std::array<wil::unique_hicon, 5> icons_;
  unsigned shownNotice_ = 0;
};

TrayIcon* g_trayIcon = nullptr;

void Wake() { State().wake.notify_all(); }

void RequestToggle() {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingWant = !(shared.pendingWant ? *shared.pendingWant : shared.view.wantOn);
  }
  Wake();
}

std::wstring ClipboardText(HWND window) {
  if (!OpenClipboard(window)) {
    return {};
  }
  std::wstring text;
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* locked = static_cast<const wchar_t*>(GlobalLock(data))) {
      text = locked;
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  const auto first = text.find_first_not_of(L" \t\r\n");
  const auto last = text.find_last_not_of(L" \t\r\n");
  return first == std::wstring::npos ? std::wstring{} : text.substr(first, last - first + 1);
}

void RequestImport(HWND window) {
  const std::wstring url = ClipboardText(window);
  if (!sovereign::tray::IsHttpsUrl(url)) {
    if (g_trayIcon != nullptr) {
      g_trayIcon->Balloon(L"Sovereign", L"В буфере обмена нет https-ссылки на подписку. Скопируй её и повтори.");
    }
    return;
  }
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingImport = Narrow(url);
  }
  Wake();
}

void RequestRefresh() {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingRefresh = true;
  }
  Wake();
}

void RequestApps(AppsChange change) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingApps = std::move(change);
  }
  Wake();
}

bool SameName(const std::string& a, const std::string& b) {
  return CompareStringOrdinal(Widen(a).c_str(), -1, Widen(b).c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Exe names of the programs with a visible window - what "add from running"
// offers - sorted, without duplicates and without the ones already listed.
std::vector<std::string> RunningApps(const std::vector<std::string>& listed) {
  std::vector<std::string> names;
  EnumWindows(
      [](HWND w, LPARAM out) -> BOOL {
        if (!IsWindowVisible(w) || GetWindowTextLengthW(w) == 0 || GetWindow(w, GW_OWNER) != nullptr) {
          return TRUE;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        const wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process || pid == GetCurrentProcessId()) {
          return TRUE;
        }
        wchar_t path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process.get(), 0, path, &size)) {
          const std::wstring full(path, size);
          // EnumWindows hands its context back as an LPARAM - the cast is the API's shape.
          reinterpret_cast<std::vector<std::string>*>(out)->push_back(  // NOLINT(performance-no-int-to-ptr)
              Narrow(full.substr(full.find_last_of(L'\\') + 1)));
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&names));
  std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
    return CompareStringOrdinal(Widen(a).c_str(), -1, Widen(b).c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  names.erase(std::unique(names.begin(), names.end(), SameName), names.end());
  std::erase_if(names, [&](const std::string& n) {
    return std::any_of(listed.begin(), listed.end(), [&](const std::string& l) { return SameName(n, l); });
  });
  if (names.size() > kMenuMaxItems) {
    names.resize(kMenuMaxItems);
  }
  return names;
}

// "Add exe...": the exe name of a file the user picks (the file dialog is
// just a convenient way to spell it right; only the name is kept).
std::optional<std::string> PickExe(HWND window) {
  try {
    const auto dialog = wil::CoCreateInstance<IFileOpenDialog>(CLSID_FileOpenDialog);
    const COMDLG_FILTERSPEC filter{L"Программы", L"*.exe"};
    THROW_IF_FAILED(dialog->SetFileTypes(1, &filter));
    THROW_IF_FAILED(dialog->SetTitle(L"Приложение для правила Sovereign"));
    const HRESULT shown = dialog->Show(window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
      return std::nullopt;
    }
    THROW_IF_FAILED(shown);
    wil::com_ptr<IShellItem> item;
    THROW_IF_FAILED(dialog->GetResult(&item));
    wil::unique_cotaskmem_string name;
    THROW_IF_FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name));
    const std::wstring full(name.get());
    return Narrow(full.substr(full.find_last_of(L'\\') + 1));
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("the file dialog failed");
    return std::nullopt;
  }
}

View CurrentView() {
  const std::scoped_lock lock(State().mutex);
  return State().view;
}

// "Из запущенных": a menu of the programs with a window, opened where the
// flyout's button was; the pick is added to the list.
void AddFromRunning(HWND window, POINT at) {
  const View view = CurrentView();
  const std::vector<std::string> running = RunningApps(view.apps);
  wil::unique_hmenu menu(CreatePopupMenu());
  if (!menu) {
    return;
  }
  for (std::size_t i = 0; i < running.size(); ++i) {
    AppendMenuW(menu.get(), MF_STRING, kMenuAddRunning + static_cast<UINT>(i), Widen(running[i]).c_str());
  }
  if (running.empty()) {
    AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, L"нет подходящих окон");
  }
  // Without the foreground switch the menu doesn't close on an outside click
  // (documented TrackPopupMenu behavior for notification-area UI).
  SetForegroundWindow(window);
  const auto command = static_cast<UINT>(
      TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0, window,
                     nullptr));
  PostMessageW(window, WM_NULL, 0, 0);
  if (command >= kMenuAddRunning && command < kMenuAddRunning + running.size()) {
    AppsChange change{view.appsMode, view.apps};
    change.list.push_back(running[command - kMenuAddRunning]);
    RequestApps(std::move(change));
  }
}

void AddExe(HWND window) {
  const View view = CurrentView();
  if (const auto exe = PickExe(window)) {
    if (std::none_of(view.apps.begin(), view.apps.end(), [&](const std::string& a) { return SameName(a, *exe); })) {
      AppsChange change{view.appsMode, view.apps};
      change.list.push_back(*exe);
      RequestApps(std::move(change));
    }
  }
}

void RequestProtocol(std::string tag) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingProtocol = std::move(tag);
  }
  Wake();
}

// What the flyout shows, from the worker's view.
sovereign::tray::FlyoutContent FlyoutFrom(const View& v) {
  sovereign::tray::FlyoutContent c;
  c.status = StatusLine(v);
  c.statusDot = sovereign::tray::StateColor(v.display);
  c.on = v.wantOn;
  c.hasSubscription = v.hasSubscription;
  if (!v.hasSubscription) {
    c.subscription = L"нет";
  } else if (!v.subscriptionError.empty()) {
    c.subscription = L"ошибка: " + Widen(v.subscriptionError);
  } else {
    c.subscription = v.lastRefresh ? LocalTime(v.lastRefresh) : L"ещё не загружена";
  }
  c.appsInclude = v.appsMode == AppsMode::Include;
  for (const std::string& app : v.apps) {
    c.apps.push_back(Widen(app));
  }
  for (const std::string& p : v.protocols) {
    c.protocols.push_back(Widen(p));
  }
  c.protocol = v.protocol;
  return c;
}

sovereign::tray::Flyout* g_flyout = nullptr;

// The tray icon's rectangle on screen (the flyout opens next to it); the
// cursor if the shell can't tell.
RECT TrayIconRect(HWND window) {
  NOTIFYICONIDENTIFIER id{};
  id.cbSize = sizeof id;
  id.hWnd = window;
  id.uID = kTrayIconId;
  RECT rect{};
  if (FAILED(Shell_NotifyIconGetRect(&id, &rect))) {
    POINT cursor{};
    GetCursorPos(&cursor);
    rect = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
  }
  return rect;
}

void OnFlyoutCommand(HWND window, sovereign::tray::FlyoutCommand command, const sovereign::tray::FlyoutArgs& args) {
  using sovereign::tray::FlyoutCommand;
  const View view = CurrentView();
  switch (command) {
    case FlyoutCommand::Toggle:
      RequestToggle();
      break;
    case FlyoutCommand::PasteSubscription:
      RequestImport(window);
      break;
    case FlyoutCommand::RefreshSubscription:
      RequestRefresh();
      break;
    case FlyoutCommand::SetAppsMode:
      RequestApps({args.index == 1 ? AppsMode::Include : AppsMode::Exclude, view.apps});
      break;
    case FlyoutCommand::RemoveApp:
      if (args.index >= 0 && static_cast<std::size_t>(args.index) < view.apps.size()) {
        AppsChange change{view.appsMode, view.apps};
        change.list.erase(change.list.begin() + args.index);
        RequestApps(std::move(change));
      }
      break;
    case FlyoutCommand::AddRunning:
      AddFromRunning(window, args.anchor);
      break;
    case FlyoutCommand::AddExe:
      AddExe(window);
      break;
    case FlyoutCommand::SetProtocol:
      if (args.index >= 0 && static_cast<std::size_t>(args.index) < view.protocols.size()) {
        RequestProtocol(view.protocols[static_cast<std::size_t>(args.index)]);
      }
      break;
    case FlyoutCommand::OpenFolder:
      try {
        ShellExecuteW(nullptr, L"open", sovereign::tray::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      } catch (...) {
        LOG_CAUGHT_EXCEPTION();
      }
      break;
    case FlyoutCommand::Exit:
      DestroyWindow(window);
      break;
  }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  if (message == taskbarCreated && g_trayIcon != nullptr) {
    g_trayIcon->Add();
    return 0;
  }
  switch (message) {
    case WM_CREATE: {
      static TrayIcon icon(window);
      g_trayIcon = &icon;
      return 0;
    }
    case kViewChangedMessage: {
      const View view = CurrentView();
      if (g_trayIcon != nullptr) {
        g_trayIcon->Update(view);
      }
      if (g_flyout != nullptr) {
        g_flyout->Update(FlyoutFrom(view));
      }
      return 0;
    }
    case kTrayCallbackMessage:
      // Either button opens the panel, like the system's own tray flyouts.
      if ((lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) && g_flyout != nullptr) {
        g_flyout->Toggle(TrayIconRect(window), FlyoutFrom(CurrentView()));
      }
      return 0;
    case WM_DESTROY:
      if (g_flyout != nullptr) {
        g_flyout->Hide();
      }
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wParam, lParam);
  }
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
  // Sharp at any scaling: the flyout sizes itself for its monitor's DPI.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // The file dialog ("add exe") is COM, on this thread.
  const auto com = wil::CoInitializeEx(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  // One tray per session: a second one would fight the first over the box.
  const wil::unique_mutex_nothrow single(CreateMutexW(nullptr, FALSE, L"Local\\SovereignTray"));
  if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
    return 0;
  }

  TraySettings settings;
  try {
    settings = sovereign::tray::LoadSettings();
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
    State().view.wantOn = settings.wantOn;
  }

  sovereign::tray::Flyout flyout(instance, [window](sovereign::tray::FlyoutCommand command,
                                                   const sovereign::tray::FlyoutArgs& args) {
    OnFlyoutCommand(window, command, args);
  });
  g_flyout = &flyout;

  int exitCode = 0;
  {
    Worker worker(std::move(settings));
    std::jthread thread([&worker](const std::stop_token& stop) { worker.Run(stop); });
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    exitCode = static_cast<int>(msg.wParam);
    thread.request_stop();
  }  // joins the worker before its state goes away
  g_flyout = nullptr;
  return exitCode;
}
