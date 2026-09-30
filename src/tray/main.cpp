// sovereign-tray: the user's side of Sovereign. A notification-area icon that
// turns the proxy on and off through the service's control pipe, shows state
// and speed, and keeps the config fresh from a subscription URL; a click on
// it opens the window (main_window.h), a right click a short menu. Decisions live in
// TrayModel and subscription.h (both unit-tested); this file is Win32 glue: a
// hidden window on the UI thread, and a worker thread that owns the settings
// and does everything that blocks (the pipe, WinHTTP) once a second or when
// the UI asks.
//
// Started by hand the tray opens its window; from the Run key (--background)
// it stays in the notification area. A second start shows the first one's
// window and exits.
//
// Quitting the tray doesn't stop the box - the service runs it, the tray only
// steers. The next tray start picks the saved on/off intent up again.

// winsock2 first: WIN32_LEAN_AND_MEAN leaves it out, iphlpapi.h needs it.
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
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
#include <deque>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "app_rules.h"
#include "autostart.h"
#include "capture.h"
#include "config_sync.h"
#include "delays.h"
#include "effective_config.h"
#include "exit_ip.h"
#include "fetch.h"
#include "protocol_choice.h"
#include "icons.h"
#include "json_field.h"
#include "log_lines.h"
#include "main_window.h"
#include "pipe_client.h"
#include "settings.h"
#include "share_links.h"
#include "sha256.h"
#include "subscription.h"
#include "tray_model.h"
#include "ui_content.h"
#include "update.h"
#include "updater.h"

namespace {

using sovereign::tray::Action;
using sovereign::tray::AppsMode;
using sovereign::tray::Display;
using sovereign::tray::FormatRate;
using sovereign::tray::UiCommand;
using sovereign::tray::UiContent;
using sovereign::tray::UiPage;
using sovereign::tray::TraySettings;
using sovereign::tray::TrayModel;

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kViewChangedMessage = WM_APP + 2;
constexpr UINT kUpdateChangedMessage = WM_APP + 3;
constexpr wchar_t kTrayClassName[] = L"SovereignTrayWindow";
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuAddRunning = 400;  // + index in RunningApps()
// The tray icon's right-click menu.
constexpr UINT kMenuToggle = 1;
constexpr UINT kMenuOpen = 2;
constexpr UINT kMenuLogs = 3;
constexpr UINT kMenuExit = 4;
constexpr std::size_t kMenuMaxItems = 250;

// What the subscription server sees: Sovereign runs the official sing-box core
// (no Mieru), so servers that pick a format by User-Agent - packetlab's does -
// give it the plain sing-box one. The version is the pinned core's.
constexpr wchar_t kUserAgent[] = L"Sovereign/" SOVEREIGN_VERSION_W L" (sing-box " SOVEREIGN_SINGBOX_VERSION_W L")";

// After a failed refresh, when to try again (the old config keeps working).
constexpr std::chrono::minutes kRefreshRetry{30};

// Log lines the tray keeps for its log window, and how many box_logs pages
// one poll reads at most (a flood is read on over the next seconds).
constexpr std::size_t kLogKeep = 5000;
constexpr int kLogPagesPerPoll = 20;

// A kill switch the service couldn't apply is tried again after this.
constexpr std::chrono::seconds kKillSwitchRetry{10};

// The exit IP: how often a running lookup is polled, and a known one rechecked.
constexpr std::chrono::seconds kExitIpPoll{2};
constexpr std::chrono::minutes kExitIpRecheck{5};

// A latency test the service never finishes stops being waited for.
constexpr std::chrono::seconds kUrlTestGiveUp{60};

// A second tray start asks the running one to show its window (registered:
// it crosses processes).
UINT ShowWindowMessage() {
  static const UINT message = RegisterWindowMessageW(L"Sovereign.ShowWindow");
  return message;
}

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

std::wstring LocalTime(std::int64_t unixSeconds) {
  const auto t = static_cast<std::time_t>(unixSeconds);
  std::tm tm{};
  localtime_s(&tm, &t);
  wchar_t text[32]{};
  wcsftime(text, 32, L"%d.%m %H:%M", &tm);
  return text;
}

// "05:07:30  ERROR  outbound/naive[Naive]: ..." - a line of the log window.
std::wstring FormatLogLine(const sovereign::tray::LogLine& line) {
  std::wstring time = L"--:--:--";
  if (line.timeMs > 0) {
    const auto t = static_cast<std::time_t>(line.timeMs / 1000);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t text[16]{};
    wcsftime(text, 16, L"%H:%M:%S", &tm);
    time = text;
  }
  std::wstring level = Widen(line.level);
  std::transform(level.begin(), level.end(), level.begin(),
                 [](wchar_t c) { return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - L'a' + L'A') : c; });
  return std::format(L"{}  {:<5}  {}", time, level, Widen(line.message));
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
  std::string subscriptionUrl;    // for "copy the link" - the UI thread's clipboard
  bool hasConfig = false;
  std::wstring subscriptionHost;  // UrlHost of the subscription URL
  std::int64_t lastRefresh = 0;
  int updateHours = 0;
  std::string subscriptionError;  // the last refresh failed with this
  bool configEdited = false;      // config.json differs from the subscription it's based on
  bool subscriptionWaiting = false;  // a newer subscription waits for the user's choice
  std::string choiceError;        // why the last choice didn't go through
  std::vector<std::string> mergeNotes;  // where the last carry-over had both sides change the same thing
  // A balloon to show once: the UI shows it when noticeId changes.
  unsigned noticeId = 0;
  std::wstring noticeTitle;
  std::wstring noticeText;
  bool noticeIsError = false;
  std::optional<UiPage> noticePage;  // what a click on the balloon opens
  AppsMode appsMode = AppsMode::Exclude;
  std::vector<std::string> apps;
  std::vector<std::string> appPaths;   // per app, "" if not known
  std::vector<std::string> protocols;  // the config selector's options
  int protocol = -1;                   // the one in use
  std::vector<std::optional<sovereign::tray::Delay>> delays;  // per protocol; nullopt: not tested
  bool delaysTesting = false;
  std::string delayError;
  sovereign::tray::ExitIp exitIp;  // through the server in use; empty while off
  bool hideExitIp = false;
  std::string logLevel;
  bool killSwitch = false;
  bool killSwitchLan = true;
  bool killSwitchActive = false;
  std::string killSwitchError;
};

// What to do with the config when a newer subscription meets the user's
// edits (config_sync.h), or to drop the edits.
enum class ConfigChoice : std::uint8_t {
  TakeNew,   // the new subscription replaces the config; the edits go to history
  KeepMine,  // the config stays; the new subscription becomes its base
  CarryOver, // the edits, merged into the new subscription
  Revert,    // back to the subscription as it arrived (the newest, if one waits)
};

// A new per-app setup from the window.
struct AppsChange {
  AppsMode mode = AppsMode::Exclude;
  std::vector<std::string> list;
  // Exe paths learned while adding (icons), name to path. A vector: MSVC's
  // debug std::map allocates when moved, so its move could throw.
  std::vector<std::pair<std::string, std::string>> paths;
};

// How often the worker looks among running processes for listed apps whose
// exe it hasn't seen yet (their icons).
constexpr std::chrono::seconds kAppPathLookup{30};

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
  std::optional<std::string> pendingLocalConfig;  // a config of the user's own (a file, the clipboard)
  bool pendingUnsubscribe = false;
  bool pendingRefresh = false;
  bool pendingUrlTest = false;
  bool pendingToggleExitIp = false;
  std::optional<std::string> pendingLogLevel;  // "" = the config's
  bool pendingToggleKillSwitch = false;
  bool pendingToggleKillSwitchLan = false;
  std::optional<AppsChange> pendingApps;
  std::optional<std::string> pendingProtocol;  // a selector option; "" = the config's own default
  std::optional<ConfigChoice> pendingChoice;
  View view;
  HWND window = nullptr;
  // The log window's lines, newest last, and how many were ever added (the
  // UI shows what came after the count it last saw).
  std::deque<std::wstring> logs;
  std::uint64_t logsAdded = 0;

  bool HasRequests() const {
    return pendingWant || pendingImport || pendingLocalConfig || pendingUnsubscribe || pendingRefresh ||
           pendingUrlTest || pendingToggleExitIp || pendingLogLevel || pendingToggleKillSwitch ||
           pendingToggleKillSwitchLan || pendingApps || pendingProtocol ||
           pendingChoice;
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
  if (sovereign::tray::Field<std::string>(json, "cmd", {}) == successCmd) {
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
  if (sovereign::tray::Field<std::string>(json, "cmd", {}) != "box_stats") {
    // "gocore not loaded" and the like: the service is up, the box can't run.
    return sovereign::tray::Stats{};
  }
  return sovereign::tray::Stats{
      .running = sovereign::tray::Field<bool>(json, "running", false),
      .uplinkBytes = sovereign::tray::Field<std::int64_t>(json, "uplink", 0),
      .downlinkBytes = sovereign::tray::Field<std::int64_t>(json, "downlink", 0),
      .connections = sovereign::tray::Field<std::int64_t>(json, "connections", 0),
      .generation = sovereign::tray::Field<std::int64_t>(json, "generation", 0),
      .configSha256 = sovereign::tray::Field<std::string>(json, "config_sha256", {}),
      .killSwitch = sovereign::tray::Field<bool>(json, "kill_switch", false),
      .killSwitchLan = sovereign::tray::Field<bool>(json, "kill_switch_lan", true),
  };
}

void Execute(TrayModel& model, Action action, const std::optional<std::string>& config) {
  if (action == Action::Start) {
    model.OnStartResult(StartBox(config), TrayModel::Clock::now());
  } else if (action == Action::Stop) {
    model.OnStopResult(StopBox());
  }
}

// Where the box keeps its cache file (cache_file.h): the service runs it as
// SYSTEM, so a machine-wide directory it can write - the one --install makes
// for the service's logs. Empty if the folder can't be found: then no cache.
std::string CacheFilePath() {
  wil::unique_cotaskmem_string programData;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) {
    return {};
  }
  return Narrow(std::wstring(programData.get()) + L"\\Sovereign\\cache.db");
}

// The worker's state besides the model: the settings it owns and saves.
class Worker {
 public:
  explicit Worker(TraySettings settings)
      : settings_(std::move(settings)), model_(settings_.wantOn), cacheFile_(CacheFilePath()) {}

  void Run(const std::stop_token& stop) {
    auto& shared = State();
    AdoptOriginal();
    while (!stop.stop_requested()) {
      std::optional<bool> want;
      std::optional<std::string> import;
      std::optional<AppsChange> apps;
      std::optional<std::string> protocol;
      std::optional<ConfigChoice> choice;
      std::optional<std::string> local;
      bool unsubscribe = false;
      bool refresh = false;
      bool urlTest = false;
      bool toggleExitIp = false;
      std::optional<std::string> logLevel;
      bool toggleKillSwitch = false;
      bool toggleKillSwitchLan = false;
      {
        const std::scoped_lock lock(shared.mutex);
        want = std::exchange(shared.pendingWant, std::nullopt);
        apps = std::exchange(shared.pendingApps, std::nullopt);
        protocol = std::exchange(shared.pendingProtocol, std::nullopt);
        choice = std::exchange(shared.pendingChoice, std::nullopt);
        import = std::exchange(shared.pendingImport, std::nullopt);
        local = std::exchange(shared.pendingLocalConfig, std::nullopt);
        unsubscribe = std::exchange(shared.pendingUnsubscribe, false);
        refresh = std::exchange(shared.pendingRefresh, false);
        urlTest = std::exchange(shared.pendingUrlTest, false);
        toggleExitIp = std::exchange(shared.pendingToggleExitIp, false);
        logLevel = std::exchange(shared.pendingLogLevel, std::nullopt);
        toggleKillSwitch = std::exchange(shared.pendingToggleKillSwitch, false);
        toggleKillSwitchLan = std::exchange(shared.pendingToggleKillSwitchLan, false);
      }
      if (want) {
        settings_.wantOn = *want;
        Save();
        Execute(model_, model_.SetWantOn(*want, TrayModel::Clock::now()), EffectiveConfig());
      }
      if (local) {
        UseLocalConfig(*local);
      }
      if (unsubscribe) {
        Unsubscribe();
      }
      if (import) {
        settings_.subscriptionUrl = *import;
        settings_.lastRefresh = 0;
        Save();
        Refresh();
      } else if (refresh || RefreshDue()) {
        Refresh();
      }
      if (choice) {
        Choose(*choice);
      }
      if (apps) {
        settings_.appsMode = apps->mode;
        settings_.apps = std::move(apps->list);
        for (auto& [name, path] : apps->paths) {
          sovereign::tray::SetAppPath(settings_, name, std::move(path));
        }
        std::erase_if(settings_.appPaths, [&](const auto& entry) {
          return std::find(settings_.apps.begin(), settings_.apps.end(), entry.first) == settings_.apps.end();
        });
        Save();  // the effective config changes: the model restarts the box
      }
      FindAppPaths();
      if (protocol) {
        settings_.protocol = *protocol;
        Save();  // likewise
      }
      const auto config = EffectiveConfig();
      model_.SetExpectedConfig(ExpectedConfigHash(config));
      const auto stats = PollStats();
      Execute(model_, model_.OnPoll(stats, TrayModel::Clock::now()), config);
      UpdateDelays(stats, urlTest);
      UpdateExitIp(stats);
      if (toggleExitIp) {
        settings_.hideExitIp = !settings_.hideExitIp;
        Save();
      }
      if (toggleKillSwitch) {
        settings_.killSwitch = !settings_.killSwitch;
        killSwitchError_.clear();
        lastKillSwitchTry_.reset();
        Save();
      }
      if (toggleKillSwitchLan) {
        settings_.killSwitchLan = !settings_.killSwitchLan;
        killSwitchError_.clear();
        lastKillSwitchTry_.reset();
        Save();
      }
      UpdateKillSwitch(stats);
      if (logLevel && *logLevel != settings_.logLevel) {
        settings_.logLevel = *logLevel;
        Save();  // the effective config changes: the model restarts the box
      }
      CollectLogs();
      Publish();

      std::unique_lock lock(shared.mutex);
      shared.wake.wait_for(lock, stop, std::chrono::seconds(1), [&] { return shared.HasRequests(); });
    }
  }

 private:
  // config.json with the protocol pick, the per-app rules and the cache file
  // applied - what the box must run.
  std::optional<std::string> EffectiveConfig() const {
    const auto config = sovereign::tray::LoadConfig();
    if (!config) {
      return std::nullopt;
    }
    return sovereign::tray::EffectiveConfig(*config, {.protocol = settings_.protocol,
                                                      .appsMode = settings_.appsMode,
                                                      .apps = settings_.apps,
                                                      .logLevel = settings_.logLevel,
                                                      .cacheFile = cacheFile_});
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

  void Notify(std::wstring title, std::wstring text, bool isError, std::optional<UiPage> page = std::nullopt) {
    ++noticeId_;
    noticeTitle_ = std::move(title);
    noticeText_ = std::move(text);
    noticeIsError_ = isError;
    noticePage_ = page;
  }

  // A tray from before subscription.json: its config.json is the
  // subscription as it arrived (refreshes overwrote it), so that's the base.
  void AdoptOriginal() {
    if (settings_.subscriptionUrl.empty()) {
      return;
    }
    try {
      if (!sovereign::tray::LoadOriginal()) {
        if (const auto config = sovereign::tray::LoadConfig()) {
          sovereign::tray::SaveOriginal(*config);
        }
      }
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("saving subscription.json failed");
    }
  }

  // config.json becomes `text`; the one it replaces goes to history.
  static void ReplaceConfig(const std::optional<std::string>& old, const std::string& text) {
    if (old && !sovereign::tray::SameConfig(*old, text)) {
      sovereign::tray::SaveHistory(*old);
    }
    sovereign::tray::SaveConfig(text);
  }

  void Refresh() {
    if (settings_.subscriptionUrl.empty()) {
      return;
    }
    auto fetched = sovereign::tray::FetchSubscription(Widen(settings_.subscriptionUrl), kUserAgent);
    std::string error;
    sovereign::tray::ConfigCheck check;
    if (!fetched) {
      error = fetched.error();
    } else {
      // Whatever the server answers with - a sing-box config, keys (a base64
      // list, as for V2Ray clients), Clash YAML, Xray JSON: when it isn't a
      // sing-box config itself, the config is made from its servers.
      if (auto made = sovereign::tray::ConfigFromSubscription(fetched->body); made.config) {
        fetched->body = std::move(*made.config);
      }
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

    // Taken whole if the config has no edits of its own; otherwise it waits
    // for the user (config_sync.h).
    using sovereign::tray::Arrival;
    Arrival arrival = Arrival::Unchanged;
    bool alreadyWaiting = false;
    try {
      const auto original = sovereign::tray::LoadOriginal();
      const auto config = sovereign::tray::LoadConfig();
      arrival = sovereign::tray::ClassifyArrival(original, config, fetched->body);
      switch (arrival) {
        case Arrival::Unchanged:
          if (!original) {
            sovereign::tray::SaveOriginal(fetched->body);
          }
          sovereign::tray::ClearPending();  // the subscription went back to what the config is based on
          break;
        case Arrival::Replace:
          ReplaceConfig(config, fetched->body);
          sovereign::tray::SaveOriginal(fetched->body);
          sovereign::tray::ClearPending();
          mergeNotes_.clear();  // about a config that's gone
          break;
        case Arrival::Ask: {
          const auto waiting = sovereign::tray::LoadPending();
          alreadyWaiting = waiting && sovereign::tray::SameConfig(*waiting, fetched->body);
          if (!alreadyWaiting) {
            sovereign::tray::SavePending(fetched->body);
          }
          break;
        }
      }
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("saving the subscription failed");
      subscriptionError_ = "не удалось сохранить конфиг в %LOCALAPPDATA%\\Sovereign";
      return;
    }
    settings_.lastRefresh = std::time(nullptr);
    settings_.updateHours =
        static_cast<int>(fetched->updateInterval.value_or(sovereign::tray::kDefaultUpdateInterval).count());
    Save();
    if (arrival == Arrival::Ask) {
      if (!alreadyWaiting) {
        Notify(L"Sovereign: новая версия подписки",
               L"В конфиге есть твои правки, поэтому он не заменён. Нажми, чтобы выбрать, что оставить.", false,
               UiPage::Subscription);
      }
      return;
    }
    Notify(L"Sovereign: подписка обновлена",
           std::format(L"выходов в конфиге: {}{}", check.outbounds,
                       arrival == Arrival::Replace ? L"" : L" (без изменений)"),
           false);
    // A running box with the old config is restarted by the model: the hash
    // the service reports no longer matches (ExpectedConfigHash).
  }

  // A config of the user's own (a file, the clipboard): it replaces the
  // config, and there's no subscription any more to replace it back.
  void UseLocalConfig(const std::string& text) {
    const auto check = sovereign::tray::CheckSubscriptionConfig(text);
    if (!check.ok) {
      Notify(L"Sovereign: конфиг не загружен", Widen(check.error), true, UiPage::Subscription);
      return;
    }
    try {
      ReplaceConfig(sovereign::tray::LoadConfig(), text);
      sovereign::tray::ClearPending();
      sovereign::tray::ClearOriginal();
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("saving the local config failed");
      Notify(L"Sovereign: конфиг не загружен", L"не удалось записать config.json", true, UiPage::Subscription);
      return;
    }
    const bool hadSubscription = !settings_.subscriptionUrl.empty();
    ForgetSubscription();
    Notify(L"Sovereign: загружен свой конфиг",
           std::format(L"выходов: {}{}", check.outbounds, hadSubscription ? L"; подписка отключена" : L""), false);
  }

  // No more refreshes; config.json stays as it is, now the user's own.
  void Unsubscribe() {
    try {
      sovereign::tray::ClearPending();
      sovereign::tray::ClearOriginal();
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("removing the subscription's files failed");
    }
    ForgetSubscription();
  }

  void ForgetSubscription() {
    settings_.subscriptionUrl.clear();
    settings_.lastRefresh = 0;
    subscriptionError_.clear();
    lastFailure_.reset();
    mergeNotes_.clear();
    Save();
  }

  void Choose(ConfigChoice choice) {
    choiceError_.clear();
    try {
      const auto original = sovereign::tray::LoadOriginal();
      const auto config = sovereign::tray::LoadConfig();
      const auto waiting = sovereign::tray::LoadPending();
      switch (choice) {
        case ConfigChoice::TakeNew:
          if (waiting) {
            ReplaceConfig(config, *waiting);
            sovereign::tray::SaveOriginal(*waiting);
            sovereign::tray::ClearPending();
            mergeNotes_.clear();
          }
          break;
        case ConfigChoice::KeepMine:
          if (waiting) {
            sovereign::tray::SaveOriginal(*waiting);
            sovereign::tray::ClearPending();
          }
          break;
        case ConfigChoice::CarryOver: {
          if (!waiting || !original || !config) {
            break;
          }
          const auto merged = sovereign::tray::MergeConfigs(*original, *config, *waiting);
          if (!merged) {
            choiceError_ = "не получилось: config.json или подписка - не JSON-объект";
            break;
          }
          const auto check = sovereign::tray::CheckSubscriptionConfig(merged->config);
          if (!check.ok) {
            choiceError_ = "после переноса правок конфиг не годится: " + check.error;
            break;
          }
          ReplaceConfig(config, merged->config);
          sovereign::tray::SaveOriginal(*waiting);
          sovereign::tray::ClearPending();
          mergeNotes_ = merged->conflicts;
          break;
        }
        case ConfigChoice::Revert: {
          const auto& to = waiting ? waiting : original;
          if (to) {
            ReplaceConfig(config, *to);
            sovereign::tray::SaveOriginal(*to);
            sovereign::tray::ClearPending();
            mergeNotes_.clear();
          }
          break;
        }
      }
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("applying the config choice failed");
      choiceError_ = "не удалось записать конфиг в %LOCALAPPDATA%\\Sovereign";
    }
    // As with a refresh: a changed config restarts the box through its hash.
  }

  // Whether config.json has edits of its own and a subscription waits -
  // re-read every poll (config.json may be edited by hand), compared only
  // when a file changed.
  void UpdateConfigState() {
    std::optional<std::string> original;
    std::optional<std::string> config;
    try {
      original = sovereign::tray::LoadOriginal();
      config = sovereign::tray::LoadConfig();
      waiting_ = sovereign::tray::LoadPending().has_value();
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("reading the config failed");
      return;
    }
    if (original != seenOriginal_ || config != seenConfig_) {
      edited_ = original && config && !sovereign::tray::SameConfig(*original, *config);
      seenOriginal_ = std::move(original);
      seenConfig_ = std::move(config);
    }
  }

  // The servers' latency: tested once every time a box starts, and whenever
  // the user asks; results polled while a test runs (the service only starts
  // one - it takes seconds, and the pipe answers one request at a time).
  void UpdateDelays(const std::optional<sovereign::tray::Stats>& stats, bool asked) {
    if (!stats) {
      generation_ = -1;  // the service is gone, and its results with it
    } else if (stats->generation != generation_) {
      generation_ = stats->generation;  // a new box: the old results are gone with the old one
      delays_.clear();
      testing_ = false;
    }
    const bool on = model_.GetDisplay() == Display::On;
    if (on && (asked || testedGeneration_ != generation_)) {
      testedGeneration_ = generation_;
      StartUrlTest();
    } else if (asked) {
      delayError_ = "подключение выключено";
    }
    if (testing_) {
      PollDelays();
    }
  }

  // The kill switch as it should be - on while the connection is meant to be
  // on - against what the service reports; told when they differ (a failure
  // is retried now and then, not every second).
  void UpdateKillSwitch(const std::optional<sovereign::tray::Stats>& stats) {
    if (!stats) {
      killSwitchActive_ = false;
      return;
    }
    killSwitchActive_ = stats->killSwitch;
    const bool enabled = settings_.killSwitch && settings_.wantOn;
    const bool differs =
        stats->killSwitch != enabled || (enabled && stats->killSwitchLan != settings_.killSwitchLan);
    if (!differs) {
      killSwitchError_.clear();
      return;
    }
    const auto now = TrayModel::Clock::now();
    if (lastKillSwitchTry_ && now - *lastKillSwitchTry_ < kKillSwitchRetry) {
      return;
    }
    lastKillSwitchTry_ = now;
    killSwitchError_ = ServiceCall(
        nlohmann::json{{"cmd", "kill_switch"}, {"enabled", enabled}, {"allow_lan", settings_.killSwitchLan}},
        "kill_switch");
    if (killSwitchError_.empty()) {
      killSwitchActive_ = enabled;
      lastKillSwitchTry_.reset();
    }
  }

  // The exit IP through the server in use: asked for when a box starts or
  // the server changes, polled while the lookup runs, and asked again now
  // and then (auto may have moved to another server).
  void UpdateExitIp(const std::optional<sovereign::tray::Stats>& stats) {
    const auto [options, current] = Protocols();
    const std::string tag = current >= 0 ? options[static_cast<std::size_t>(current)] : std::string();
    if (!stats || model_.GetDisplay() != Display::On || tag.empty()) {
      exit_ = {};
      exitTag_.clear();
      exitAsked_.reset();
      return;
    }
    const auto now = TrayModel::Clock::now();
    const bool fresh = tag != exitTag_ || stats->generation != exitGeneration_;
    const auto wait = exit_.pending ? kExitIpPoll : kExitIpRecheck;
    if (!fresh && exitAsked_ && now - *exitAsked_ < wait) {
      return;
    }
    const bool refresh = fresh || !exit_.pending;  // a poll of a running lookup doesn't restart it
    exitTag_ = tag;
    exitGeneration_ = stats->generation;
    exitAsked_ = now;
    const auto response = sovereign::tray::RequestService(
        nlohmann::json{{"cmd", "box_exitip"}, {"tag", tag}, {"refresh", refresh}}.dump());
    if (const auto parsed = response ? sovereign::tray::ParseExitIpResponse(*response) : std::nullopt) {
      // A failed recheck keeps the last known address: it's still the best guess.
      if (parsed->ip.empty() && !parsed->pending && !exit_.ip.empty() && !fresh) {
        return;
      }
      exit_ = *parsed;
    }
  }

  void StartUrlTest() {
    const std::vector<std::string> tags = Protocols().first;
    if (tags.empty()) {
      return;
    }
    const std::string error = ServiceCall(nlohmann::json{{"cmd", "box_urltest"}, {"tags", tags}}, "box_urltest_started");
    delayError_ = error;
    if (!error.empty()) {
      return;
    }
    testing_ = true;
    testStarted_ = TrayModel::Clock::now();
    for (const std::string& tag : tags) {
      delays_[tag] = sovereign::tray::Delay{};  // pending until the service says
    }
  }

  void PollDelays() {
    const auto response = sovereign::tray::RequestService(R"({"cmd":"box_delays"})");
    if (const auto results = response ? sovereign::tray::ParseDelaysResponse(*response) : std::nullopt) {
      for (const auto& [tag, delay] : *results) {
        delays_[tag] = delay;
      }
    }
    const bool pending = std::any_of(delays_.begin(), delays_.end(), [](const auto& entry) {
      return entry.second.state == sovereign::tray::Delay::State::Pending;
    });
    if (!pending || TrayModel::Clock::now() - testStarted_ > kUrlTestGiveUp) {
      testing_ = false;
    }
  }

  // The core's new log lines from the service, plus the tray's own errors
  // (a box that never started has no log of its own to explain why), into the
  // lines the main window's log shows.
  void CollectLogs() {
    std::vector<std::wstring> fresh;
    for (int page = 0; page < kLogPagesPerPoll; ++page) {
      const auto response =
          sovereign::tray::RequestService(nlohmann::json{{"cmd", "box_logs"}, {"since", logSince_}}.dump());
      const auto parsed = response ? sovereign::tray::ParseLogsResponse(*response) : std::nullopt;
      if (!parsed) {
        break;
      }
      for (const auto& line : parsed->lines) {
        fresh.push_back(FormatLogLine(line));
      }
      logSince_ = parsed->next;
      if (!parsed->more) {
        break;
      }
    }
    if (const std::string& error = model_.LastError(); error != loggedError_) {
      loggedError_ = error;
      if (!error.empty()) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        fresh.push_back(FormatLogLine(
            {std::chrono::duration_cast<std::chrono::milliseconds>(now).count(), "error", "трей: " + error}));
      }
    }
    if (fresh.empty()) {
      return;
    }
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    for (auto& line : fresh) {
      shared.logs.push_back(std::move(line));
    }
    while (shared.logs.size() > kLogKeep) {
      shared.logs.pop_front();
    }
    shared.logsAdded += fresh.size();
  }

  // Listed apps whose exe hasn't been seen: looked for among the running
  // processes now and then, so their icons show once they've run.
  void FindAppPaths() {
    const auto now = TrayModel::Clock::now();
    if (lastPathLookup_ && now - *lastPathLookup_ < kAppPathLookup) {
      return;
    }
    lastPathLookup_ = now;
    std::vector<std::wstring> missing;
    for (const std::string& app : settings_.apps) {
      if (sovereign::tray::AppPath(settings_, app) == nullptr) {
        missing.push_back(Widen(app));
      }
    }
    if (missing.empty()) {
      return;
    }
    const wil::unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) {
      return;
    }
    bool found = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
      const auto match = std::find_if(missing.begin(), missing.end(), [&](const std::wstring& name) {
        return CompareStringOrdinal(name.c_str(), -1, entry.szExeFile, -1, TRUE) == CSTR_EQUAL;
      });
      if (match == missing.end()) {
        continue;
      }
      const wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
      wchar_t path[MAX_PATH]{};
      DWORD size = MAX_PATH;
      if (process && QueryFullProcessImageNameW(process.get(), 0, path, &size)) {
        sovereign::tray::SetAppPath(settings_, Narrow(*match), Narrow(std::wstring(path, size)));
        missing.erase(match);
        found = true;
      }
    }
    if (found) {
      Save();
    }
  }

  void Publish() {
    auto& shared = State();
    auto protocols = Protocols();  // reads config.json: outside the lock
    UpdateConfigState();
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
      v.subscriptionUrl = settings_.subscriptionUrl;
      v.hasConfig = seenConfig_.has_value();
      v.subscriptionHost = sovereign::tray::UrlHost(Widen(settings_.subscriptionUrl));
      v.lastRefresh = settings_.lastRefresh;
      v.updateHours = settings_.updateHours;
      v.subscriptionError = subscriptionError_;
      v.configEdited = edited_;
      v.subscriptionWaiting = waiting_;
      v.choiceError = choiceError_;
      v.mergeNotes = mergeNotes_;
      v.noticeId = noticeId_;
      v.noticeTitle = noticeTitle_;
      v.noticeText = noticeText_;
      v.noticeIsError = noticeIsError_;
      v.noticePage = noticePage_;
      v.appsMode = settings_.appsMode;
      v.apps = settings_.apps;
      v.appPaths.clear();
      for (const std::string& app : settings_.apps) {
        const std::string* path = sovereign::tray::AppPath(settings_, app);
        v.appPaths.push_back(path != nullptr ? *path : std::string());
      }
      std::tie(v.protocols, v.protocol) = protocols;
      v.delays.clear();
      for (const std::string& tag : v.protocols) {
        const auto it = delays_.find(tag);
        v.delays.push_back(it == delays_.end() ? std::nullopt : std::optional(it->second));
      }
      v.delaysTesting = testing_;
      v.delayError = delayError_;
      v.exitIp = exit_;
      v.hideExitIp = settings_.hideExitIp;
      v.logLevel = settings_.logLevel;
      v.killSwitch = settings_.killSwitch;
      v.killSwitchLan = settings_.killSwitchLan;
      v.killSwitchActive = killSwitchActive_;
      v.killSwitchError = killSwitchError_;
      window = shared.window;
    }
    PostMessageW(window, kViewChangedMessage, 0, 0);
  }

  TraySettings settings_;
  TrayModel model_;
  std::string cacheFile_;  // UTF-8; empty: no cache file
  std::optional<TrayModel::Clock::time_point> lastFailure_;
  std::string subscriptionError_;
  unsigned noticeId_ = 0;
  std::wstring noticeTitle_;
  std::wstring noticeText_;
  bool noticeIsError_ = false;
  std::optional<UiPage> noticePage_;
  std::optional<TrayModel::Clock::time_point> lastPathLookup_;
  bool edited_ = false;
  bool waiting_ = false;
  std::optional<std::string> seenOriginal_;  // the texts edited_ was computed from
  std::optional<std::string> seenConfig_;
  std::string choiceError_;
  std::vector<std::string> mergeNotes_;
  std::uint64_t logSince_ = 0;  // box_logs cursor
  std::string loggedError_;     // the model's error last put into the log
  std::map<std::string, sovereign::tray::Delay> delays_;  // by outbound tag
  bool testing_ = false;
  TrayModel::Clock::time_point testStarted_{};
  std::int64_t generation_ = -1;        // the box the results belong to
  std::int64_t testedGeneration_ = -1;  // the box last tested automatically
  std::string delayError_;
  sovereign::tray::ExitIp exit_;
  std::string exitTag_;               // the server exit_ is about
  std::int64_t exitGeneration_ = -1;  // and the box
  std::optional<TrayModel::Clock::time_point> exitAsked_;
  bool killSwitchActive_ = false;
  std::string killSwitchError_;
  std::optional<TrayModel::Clock::time_point> lastKillSwitchTry_;
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

  // Gone from the notification area now, not when the process ends.
  void Remove() { Shell_NotifyIconW(NIM_DELETE, &data_); }

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
  void Balloon(const std::wstring& title, const std::wstring& text, DWORD flags = NIIF_WARNING) {
    data_.uFlags = NIF_INFO;
    data_.dwInfoFlags = flags;
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

void RequestLocalConfig(std::string text) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingLocalConfig = std::move(text);
  }
  Wake();
}

void ImportBalloon(const std::wstring& message) {
  if (g_trayIcon != nullptr) {
    g_trayIcon->Balloon(L"Sovereign", message);
  }
}

constexpr const wchar_t* kNothingToImport =
    L"Не нашёл ни ссылки на подписку, ни конфига, ни ключей (vless://, vmess://, trojan://, ss://, hy2://, tuic://, "
    L"anytls://, wireguard://...), ни Clash/Xray-конфига.";

// What was pasted, read from a file or from QR codes, whatever it is
// (share_links.h): a subscription link, a whole config, servers in any of
// the shapes and formats it knows - made into a config. False when nothing
// in it was recognized; `quiet` leaves saying so to the caller, who may
// look elsewhere (a picture next to the text on the clipboard).
bool ImportText(const std::string& text, bool quiet = false) {
  if (text.size() > sovereign::tray::kMaxSubscriptionBytes) {
    ImportBalloon(L"Слишком большой текст: больше 4 МБ.");
    return true;
  }
  const auto items = sovereign::tray::RecognizeImport(text);
  if (items.json) {
    RequestLocalConfig(*items.json);  // the worker checks it, and says why not
    return true;
  }
  if (items.Servers() || !items.skipped.empty()) {
    const auto built = sovereign::tray::BuildConfig(items);
    const std::wstring why = Widen(built.errors.empty() ? std::string() : built.errors.front());
    if (!built.config) {
      ImportBalloon(L"Ни один сервер не подошёл: " + why);
      return true;
    }
    if (!built.errors.empty()) {
      ImportBalloon(std::format(L"Взято серверов: {} из {}. {}", built.servers, built.found, why));
    }
    RequestLocalConfig(*built.config);
    return true;
  }
  for (const std::string& url : items.urls) {
    if (sovereign::tray::IsHttpsUrl(Widen(url))) {
      {
        auto& shared = State();
        const std::scoped_lock lock(shared.mutex);
        shared.pendingImport = url;
      }
      Wake();
      return true;
    }
  }
  if (!items.urls.empty()) {
    ImportBalloon(L"Подписка берётся только по https://, а тут " + Widen(items.urls.front()));
    return true;
  }
  if (!items.jsonError.empty()) {
    ImportBalloon(L"Конфиг с ошибкой: " + Widen(items.jsonError));
    return true;
  }
  if (!quiet) {
    ImportBalloon(kNothingToImport);
  }
  return false;
}

// What QR codes hold, together: several on one screen may be several keys.
bool ImportQrCodes(const std::vector<std::string>& codes, bool quiet) {
  std::string text;
  for (const std::string& code : codes) {
    text += (text.empty() ? "" : "\n") + code;
  }
  if (ImportText(text, /*quiet=*/true)) {
    return true;
  }
  if (!quiet) {
    std::wstring shown = Widen(codes.front());
    if (shown.size() > 60) {
      shown = shown.substr(0, 60) + L"...";
    }
    ImportBalloon(L"QR-код прочитан, но в нём не ключ и не ссылка: " + shown);
  }
  return false;
}

// A file: a picture's QR codes, or a text (a config, keys, YAML, a link).
bool ImportPath(const std::wstring& path, bool quiet) {
  if (sovereign::tray::IsImageFile(path)) {
    const auto codes = sovereign::tray::ImageFileQrCodes(path);
    if (codes.empty()) {
      if (!quiet) {
        ImportBalloon(L"В картинке не нашёл QR-кода.");
      }
      return false;
    }
    return ImportQrCodes(codes, quiet);
  }
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  std::string text;
  if (in) {
    text.resize(sovereign::tray::kMaxSubscriptionBytes + 1);
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
  }
  if (!in.eof() && !in) {
    if (!quiet) {
      ImportBalloon(L"Не удалось прочитать файл.");
    }
    return false;
  }
  return ImportText(text, quiet);
}

// "Paste": whatever the clipboard holds - text, a picture with QR codes
// (a screenshot, an image copied from a browser or a messenger), files
// copied in Explorer.
void RequestImport(HWND window) {
  const std::wstring text = ClipboardText(window);
  // A path copied as text (Explorer's "Copy as path"): the file it names.
  if (!text.empty() && text.find_first_of(L"\r\n") == std::wstring::npos) {
    std::wstring path = text;
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
      path = path.substr(1, path.size() - 2);
    }
    std::error_code error;
    if (std::filesystem::is_regular_file(std::filesystem::path(path), error)) {
      ImportPath(path, /*quiet=*/false);
      return;
    }
  }
  if (!text.empty() && ImportText(Narrow(text), /*quiet=*/true)) {
    return;
  }
  const bool picture = IsClipboardFormatAvailable(CF_BITMAP) != FALSE;
  if (const auto codes = sovereign::tray::ClipboardQrCodes(window); !codes.empty()) {
    ImportQrCodes(codes, /*quiet=*/false);
    return;
  }
  for (const std::wstring& path : sovereign::tray::ClipboardFiles(window)) {
    if (ImportPath(path, /*quiet=*/true)) {
      return;
    }
  }
  ImportBalloon(picture && text.empty() ? L"В картинке из буфера не нашёл QR-кода."
                : text.empty() && !picture ? L"Буфер пуст: скопируйте ссылку, ключи, конфиг или картинку с QR-кодом."
                                          : kNothingToImport);
}

// "QR from the screen": every monitor as it is now - a code in a browser, a
// messenger, a video.
void ScanScreen() {
  const HCURSOR before = SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));  // IDC_WAIT
  const auto codes = sovereign::tray::ScreenQrCodes();
  SetCursor(before);
  if (codes.empty()) {
    ImportBalloon(L"На экране не нашёл QR-кода: откройте его так, чтобы он был виден целиком.");
    return;
  }
  ImportQrCodes(codes, /*quiet=*/false);
}

// "File...": a config, keys, a link, YAML, or a picture with a QR code, from disk.
void ImportFile(HWND window) {
  std::wstring path;
  try {
    const auto dialog = wil::CoCreateInstance<IFileOpenDialog>(CLSID_FileOpenDialog);
    const std::array<COMDLG_FILTERSPEC, 2> filters{
        {{L"Конфиг, ключи или QR-код",
          L"*.json;*.txt;*.conf;*.yaml;*.yml;*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.heic"},
         {L"Все файлы", L"*.*"}}};
    THROW_IF_FAILED(dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data()));
    THROW_IF_FAILED(dialog->SetTitle(L"Конфиг, ключи, ссылка или картинка с QR-кодом для Sovereign"));
    const HRESULT shown = dialog->Show(window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
      return;
    }
    THROW_IF_FAILED(shown);
    wil::com_ptr<IShellItem> item;
    THROW_IF_FAILED(dialog->GetResult(&item));
    wil::unique_cotaskmem_string name;
    THROW_IF_FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name));
    path = name.get();
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("the file dialog failed");
    return;
  }
  ImportPath(path, /*quiet=*/false);
}

bool CopyText(HWND owner, const std::wstring& text) {
  if (!OpenClipboard(owner)) {
    return false;
  }
  bool copied = false;
  if (EmptyClipboard()) {
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    wil::unique_hglobal memory(GlobalAlloc(GMEM_MOVEABLE, bytes));
    if (memory) {
      if (void* locked = GlobalLock(memory.get()); locked != nullptr) {
        std::memcpy(locked, text.c_str(), bytes);
        GlobalUnlock(memory.get());
        if (SetClipboardData(CF_UNICODETEXT, memory.get()) != nullptr) {
          memory.release();  // the clipboard owns it now
          copied = true;
        }
      }
    }
  }
  CloseClipboard();
  return copied;
}

void RequestUnsubscribe() {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingUnsubscribe = true;
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

struct RunningApp {
  std::string name;  // the exe's, what the rule matches
  std::string path;  // the exe's full path, for its icon
};

// The programs with a visible window - what "add from running" offers -
// sorted by name, without duplicates and without the ones already listed.
std::vector<RunningApp> RunningApps(const std::vector<std::string>& listed) {
  std::vector<RunningApp> names;
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
          reinterpret_cast<std::vector<RunningApp>*>(out)->push_back(  // NOLINT(performance-no-int-to-ptr)
              {Narrow(full.substr(full.find_last_of(L'\\') + 1)), Narrow(full)});
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&names));
  std::sort(names.begin(), names.end(), [](const RunningApp& a, const RunningApp& b) {
    return CompareStringOrdinal(Widen(a.name).c_str(), -1, Widen(b.name).c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  names.erase(std::unique(names.begin(), names.end(),
                          [](const RunningApp& a, const RunningApp& b) { return SameName(a.name, b.name); }),
              names.end());
  std::erase_if(names, [&](const RunningApp& n) {
    return std::any_of(listed.begin(), listed.end(), [&](const std::string& l) { return SameName(n.name, l); });
  });
  if (names.size() > kMenuMaxItems) {
    names.resize(kMenuMaxItems);
  }
  return names;
}

// "Add exe...": the full path of an exe the user picks (the rule keeps only
// its name; the path is for the icon).
std::optional<std::wstring> PickExe(HWND window) {
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
    return std::wstring(name.get());
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("the file dialog failed");
    return std::nullopt;
  }
}

View CurrentView() {
  const std::scoped_lock lock(State().mutex);
  return State().view;
}

// "Из запущенных": a menu of the programs with a window, opened under the
// window's button; the pick is added to the list.
void AddFromRunning(HWND window, POINT at) {
  const View view = CurrentView();
  const std::vector<RunningApp> running = RunningApps(view.apps);
  wil::unique_hmenu menu(CreatePopupMenu());
  if (!menu) {
    return;
  }
  for (std::size_t i = 0; i < running.size(); ++i) {
    AppendMenuW(menu.get(), MF_STRING, kMenuAddRunning + static_cast<UINT>(i), Widen(running[i].name).c_str());
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
    const RunningApp& pick = running[command - kMenuAddRunning];
    AppsChange change{view.appsMode, view.apps, {{pick.name, pick.path}}};
    change.list.push_back(pick.name);
    RequestApps(std::move(change));
  }
}

void AddExe(HWND window) {
  const View view = CurrentView();
  if (const auto path = PickExe(window)) {
    const std::string exe = Narrow(path->substr(path->find_last_of(L'\\') + 1));
    if (std::none_of(view.apps.begin(), view.apps.end(), [&](const std::string& a) { return SameName(a, exe); })) {
      AppsChange change{view.appsMode, view.apps, {{exe, Narrow(*path)}}};
      change.list.push_back(exe);
      RequestApps(std::move(change));
    }
  }
}

void RequestUrlTest() {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingUrlTest = true;
  }
  Wake();
}

void RequestProtocol(std::string tag) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingProtocol = std::move(tag);
  }
  Wake();
}

void RequestChoice(ConfigChoice choice) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingChoice = choice;
  }
  Wake();
}

// A country's name as the system says it ("Нидерланды" on a Russian
// Windows); the code itself if it doesn't know it.
std::wstring CountryName(const std::wstring& code) {
  if (code.size() != 2) {
    return code;
  }
  // Windows 10 1709+, and the SDK declares it only when built for that:
  // looked up at run time instead.
  using GetGeoInfoExFn = int(WINAPI*)(PWSTR, GEOTYPE, PWSTR, int);
  // Through an integer, as go_core.cpp's ResolveExport: clang rejects a
  // direct FARPROC-to-function cast.
  static const auto getGeoInfoEx = reinterpret_cast<GetGeoInfoExFn>(  // NOLINT(performance-no-int-to-ptr)
      reinterpret_cast<std::uintptr_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetGeoInfoEx")));
  if (getGeoInfoEx == nullptr) {
    return code;
  }
  std::wstring location = code;
  wchar_t name[128]{};
  const int n = getGeoInfoEx(location.data(), GEO_FRIENDLYNAME, name, static_cast<int>(std::size(name)));
  return n > 1 ? std::wstring(name) : code;
}

// "запись: …": a menu of what the core may write to the log; a pick restarts
// the box (the effective config changes).
void ChooseLogLevel(HWND window, POINT at) {
  const View view = CurrentView();
  wil::unique_hmenu menu(CreatePopupMenu());
  if (!menu) {
    return;
  }
  constexpr UINT kFirst = 1;
  static constexpr std::array<std::pair<std::string_view, const wchar_t*>, 5> kItems = {{
      {"", L"Как в конфиге"},
      {"debug", L"Всё, с отладкой (debug)"},
      {"info", L"Обычное (info)"},
      {"warn", L"Предупреждения и ошибки (warn)"},
      {"error", L"Только ошибки (error)"},
  }};
  for (std::size_t i = 0; i < kItems.size(); ++i) {
    const UINT checked = kItems[i].first == view.logLevel ? MF_CHECKED : MF_UNCHECKED;
    AppendMenuW(menu.get(), MF_STRING | checked, kFirst + static_cast<UINT>(i), kItems[i].second);
  }
  AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, L"Смена перезапускает подключение");
  SetForegroundWindow(window);
  const auto command = static_cast<UINT>(
      TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0, window,
                     nullptr));
  PostMessageW(window, WM_NULL, 0, 0);
  if (command >= kFirst && command < kFirst + kItems.size()) {
    {
      auto& shared = State();
      const std::scoped_lock lock(shared.mutex);
      shared.pendingLogLevel = std::string(kItems[command - kFirst].first);
    }
    Wake();
  }
}

void RequestToggleExitIp() {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingToggleExitIp = true;
  }
  Wake();
}

sovereign::tray::Updater* g_updater = nullptr;  // the UI thread's; the updater has its own

// What the window shows, from the worker's view.
UiContent ContentFrom(const View& v) {
  UiContent c;
  c.display = v.display;
  c.on = v.wantOn;
  c.down = v.down;
  c.up = v.up;
  c.connections = v.connections;
  c.error = v.display == Display::Error ? Widen(v.error) : std::wstring();
  c.hasSubscription = v.hasSubscription;
  c.hasConfig = v.hasConfig;
  c.subscriptionHost = v.subscriptionHost;
  c.subscriptionError = Widen(v.subscriptionError);
  c.updateHours = v.hasSubscription ? v.updateHours : 0;
  c.configEdited = v.configEdited;
  c.subscriptionWaiting = v.subscriptionWaiting;
  c.choiceError = Widen(v.choiceError);
  for (const std::string& note : v.mergeNotes) {
    c.mergeNotes.push_back(Widen(note));
  }
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
  for (const std::string& path : v.appPaths) {
    c.appPaths.push_back(Widen(path));
  }
  for (const std::string& p : v.protocols) {
    c.protocols.push_back(Widen(p));
  }
  c.protocol = v.protocol;
  for (const auto& delay : v.delays) {
    sovereign::tray::UiDelay d;
    if (delay) {
      using State = sovereign::tray::Delay::State;
      d.state = delay->state == State::Ok        ? sovereign::tray::UiDelay::State::Ok
                : delay->state == State::Failed ? sovereign::tray::UiDelay::State::Failed
                                                 : sovereign::tray::UiDelay::State::Pending;
      d.ms = delay->ms;
    }
    c.delays.push_back(d);
  }
  c.delaysTesting = v.delaysTesting;
  c.canTestDelays = v.display == Display::On && !v.protocols.empty();
  c.delayError = Widen(v.delayError);
  c.exitIp = Widen(v.exitIp.ip);
  c.exitCountry = Widen(v.exitIp.country);
  c.exitCountryName = CountryName(c.exitCountry);
  c.exitPending = v.exitIp.pending;
  c.hideExitIp = v.hideExitIp;
  c.logLevel = Widen(v.logLevel);
  c.killSwitch = v.killSwitch;
  c.killSwitchLan = v.killSwitchLan;
  c.killSwitchActive = v.killSwitchActive;
  c.killSwitchError = Widen(v.killSwitchError);
  c.autostart = sovereign::tray::AutostartEnabled();
  c.version = SOVEREIGN_VERSION_W L" · sing-box " SOVEREIGN_SINGBOX_VERSION_W;
  if (g_updater != nullptr) {
    using Status = sovereign::tray::Updater::Status;
    const auto update = g_updater->Get();
    switch (update.status) {
      case Status::Idle: c.update = sovereign::tray::UiUpdate::Idle; break;
      case Status::Checking: c.update = sovereign::tray::UiUpdate::Checking; break;
      case Status::UpToDate: c.update = sovereign::tray::UiUpdate::UpToDate; break;
      case Status::Available: c.update = sovereign::tray::UiUpdate::Available; break;
      case Status::Downloading:
      case Status::Ready: c.update = sovereign::tray::UiUpdate::Downloading; break;
      case Status::Failed: c.update = sovereign::tray::UiUpdate::Failed; break;
    }
    c.updateVersion = Widen(update.latest);
    c.updateError = Widen(update.error);
  }
  return c;
}

sovereign::tray::MainWindow* g_mainWindow = nullptr;
std::string g_announcedUpdate;       // the version the balloon already told about
std::optional<UiPage> g_balloonPage;  // what a click on the last balloon opens
bool g_installerLaunched = false;
unsigned g_lastNotice = 0;
std::uint64_t g_logsShown = 0;  // Shared::logsAdded as of the main window's last log update

// The log lines added since the main window's log was last updated. The
// window keeps them whether it's shown or not, so opening it is instant.
void UpdateLogs() {
  if (g_mainWindow == nullptr) {
    return;
  }
  std::vector<std::wstring> lines;
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    const auto fresh = static_cast<std::size_t>(std::min<std::uint64_t>(shared.logsAdded - g_logsShown, shared.logs.size()));
    lines.assign(shared.logs.end() - static_cast<std::ptrdiff_t>(fresh), shared.logs.end());
    g_logsShown = shared.logsAdded;
  }
  g_mainWindow->AppendLogs(lines);
}

void UpdateWindows() {
  const UiContent content = ContentFrom(CurrentView());
  if (g_mainWindow != nullptr) {
    g_mainWindow->Update(content);
  }
}

void ShowMainWindow(UiPage page) {
  if (g_mainWindow != nullptr) {
    g_mainWindow->Update(ContentFrom(CurrentView()));
    g_mainWindow->Show(page);
  }
}

// A command from the window. `trayWindow` is the hidden
// window that owns the icon; menus and dialogs belong to args.owner if set.
void OnUiCommand(HWND trayWindow, UiCommand command, const sovereign::tray::UiArgs& args) {
  const View view = CurrentView();
  HWND owner = args.owner != nullptr ? args.owner : trayWindow;
  switch (command) {
    case UiCommand::Toggle:
      RequestToggle();
      break;
    case UiCommand::PasteSubscription:
      RequestImport(owner);
      break;
    case UiCommand::RefreshSubscription:
      RequestRefresh();
      break;
    case UiCommand::TakeSubscription:
      RequestChoice(ConfigChoice::TakeNew);
      break;
    case UiCommand::KeepConfig:
      RequestChoice(ConfigChoice::KeepMine);
      break;
    case UiCommand::ImportFile:
      ImportFile(owner);
      break;
    case UiCommand::ScanScreen:
      ScanScreen();
      break;
    case UiCommand::CopySubscription:
      if (!view.subscriptionUrl.empty() && CopyText(owner, Widen(view.subscriptionUrl)) && g_trayIcon != nullptr) {
        g_trayIcon->Balloon(L"Sovereign: ссылка скопирована",
                            L"В ней твой ключ доступа - отдавай только своим устройствам.", NIIF_INFO);
      }
      break;
    case UiCommand::RemoveSubscription:
      if (MessageBoxW(owner,
                      L"Отключить подписку?\n\nКонфиг останется как есть и будет работать, но обновляться "
                      L"больше не будет. Подключить снова - вставить ссылку.",
                      L"Sovereign", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
        RequestUnsubscribe();
      }
      break;
    case UiCommand::CarryOverEdits:
      RequestChoice(ConfigChoice::CarryOver);
      break;
    case UiCommand::RevertConfig:
      if (MessageBoxW(owner,
                      L"Вернуть конфиг к подписке в том виде, как она пришла?\n\n"
                      L"Твои правки не пропадут: нынешний config.json сохранится в папке history.",
                      L"Sovereign", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
        RequestChoice(ConfigChoice::Revert);
      }
      break;
    case UiCommand::SetAppsMode:
      RequestApps({args.index == 1 ? AppsMode::Include : AppsMode::Exclude, view.apps, {}});
      break;
    case UiCommand::RemoveApp:
      if (args.index >= 0 && static_cast<std::size_t>(args.index) < view.apps.size()) {
        AppsChange change{view.appsMode, view.apps};
        change.list.erase(change.list.begin() + args.index);
        RequestApps(std::move(change));
      }
      break;
    case UiCommand::AddRunning:
      AddFromRunning(owner, args.anchor);
      break;
    case UiCommand::AddExe:
      AddExe(owner);
      break;
    case UiCommand::SetProtocol:
      if (args.index >= 0 && static_cast<std::size_t>(args.index) < view.protocols.size()) {
        RequestProtocol(view.protocols[static_cast<std::size_t>(args.index)]);
      }
      break;
    case UiCommand::ToggleAutostart:
      if (!sovereign::tray::SetAutostart(!sovereign::tray::AutostartEnabled()) && g_trayIcon != nullptr) {
        g_trayIcon->Balloon(L"Sovereign", L"Не удалось изменить автозапуск (реестр отказал).");
      }
      UpdateWindows();
      break;
    case UiCommand::ToggleKillSwitch:
    case UiCommand::ToggleKillSwitchLan: {
      {
        auto& shared = State();
        const std::scoped_lock lock(shared.mutex);
        (command == UiCommand::ToggleKillSwitch ? shared.pendingToggleKillSwitch : shared.pendingToggleKillSwitchLan) =
            true;
      }
      Wake();
      break;
    }
    case UiCommand::ChooseLogLevel:
      ChooseLogLevel(owner, args.anchor);
      break;
    case UiCommand::ToggleExitIp:
      RequestToggleExitIp();
      break;
    case UiCommand::TestDelays:
      RequestUrlTest();
      break;
    case UiCommand::CheckUpdate:
      if (g_updater != nullptr) {
        g_updater->Check();
      }
      break;
    case UiCommand::InstallUpdate:
      if (g_updater != nullptr) {
        g_updater->Install();
      }
      break;
    case UiCommand::OpenWindow:
      ShowMainWindow(args.index >= 0 && args.index < sovereign::tray::kUiPageCount ? static_cast<UiPage>(args.index)
                                                                                   : UiPage::Overview);
      break;
    case UiCommand::OpenFolder:
      try {
        ShellExecuteW(nullptr, L"open", sovereign::tray::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      } catch (...) {
        LOG_CAUGHT_EXCEPTION();
      }
      break;
    case UiCommand::Exit:
      DestroyWindow(trayWindow);
      break;
  }
}

// The tray icon's right-click menu: what can't wait for the window.
void ShowTrayMenu(HWND window) {
  const View view = CurrentView();
  wil::unique_hmenu menu(CreatePopupMenu());
  if (!menu) {
    return;
  }
  AppendMenuW(menu.get(), MF_STRING, kMenuToggle, view.wantOn ? L"Выключить" : L"Включить");
  AppendMenuW(menu.get(), MF_STRING, kMenuOpen, L"Открыть");
  AppendMenuW(menu.get(), MF_STRING, kMenuLogs, L"Журнал");
  AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu.get(), MF_STRING, kMenuExit, L"Выход");
  SetMenuDefaultItem(menu.get(), kMenuOpen, FALSE);
  POINT at{};
  GetCursorPos(&at);
  // Without the foreground switch the menu doesn't close on an outside click.
  SetForegroundWindow(window);
  const auto command = static_cast<UINT>(TrackPopupMenu(
      menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN, at.x, at.y, 0,
      window, nullptr));
  PostMessageW(window, WM_NULL, 0, 0);
  switch (command) {
    case kMenuToggle: RequestToggle(); break;
    case kMenuOpen: ShowMainWindow(UiPage::Overview); break;
    case kMenuLogs: ShowMainWindow(UiPage::Logs); break;
    case kMenuExit: DestroyWindow(window); break;
    default: break;
  }
}

// The updater moved on: tell about a new version once, and run a downloaded
// installer - which replaces this tray, so the tray makes way.
void OnUpdateChanged(HWND trayWindow) {
  if (g_updater == nullptr) {
    return;
  }
  const auto update = g_updater->Get();
  using Status = sovereign::tray::Updater::Status;
  if (update.status == Status::Available && update.latest != g_announcedUpdate && g_trayIcon != nullptr) {
    g_announcedUpdate = update.latest;
    g_trayIcon->Balloon(L"Sovereign: доступна версия " + Widen(update.latest),
                        L"Нажми, чтобы обновить - установщик скачается с GitHub.", NIIF_INFO);
    g_balloonPage = UiPage::Settings;
  }
  if (update.status == Status::Ready && !g_installerLaunched) {
    HWND owner = g_mainWindow != nullptr && g_mainWindow->IsVisible() ? GetForegroundWindow() : trayWindow;
    const std::string error = sovereign::tray::LaunchInstaller(owner, update.installer);
    if (error.empty()) {
      g_installerLaunched = true;
      DestroyWindow(trayWindow);  // the installer starts the new tray when it's done
      return;
    }
    g_updater->LaunchFailed(error);
  }
  UpdateWindows();
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  if (message == taskbarCreated && g_trayIcon != nullptr) {
    g_trayIcon->Add();
    return 0;
  }
  if (message == ShowWindowMessage() && message != 0) {
    ShowMainWindow(UiPage::Overview);
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
        if (view.noticeId != 0 && view.noticeId != g_lastNotice) {
          g_lastNotice = view.noticeId;
          g_balloonPage = view.noticePage;  // this balloon is the worker's
        }
        g_trayIcon->Update(view);
      }
      UpdateWindows();
      UpdateLogs();
      return 0;
    }
    case kUpdateChangedMessage:
      OnUpdateChanged(window);
      return 0;
    case kTrayCallbackMessage:
      // A click shows the window or hides it; a right click is the menu.
      if (lParam == NIN_BALLOONUSERCLICK && g_balloonPage) {
        ShowMainWindow(*g_balloonPage);
      } else if (lParam == WM_LBUTTONUP && g_mainWindow != nullptr) {
        g_mainWindow->Update(ContentFrom(CurrentView()));
        g_mainWindow->Toggle();
      } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
        ShowTrayMenu(window);
      }
      return 0;
    case WM_DESTROY:
      // Everything visible goes first: after the message loop the worker is
      // joined, and it may be inside a pipe call that takes a while (a
      // box_start waiting for the network: 10 s) - with the icon and windows
      // still up, Windows reported the tray as hung for that long.
      if (g_trayIcon != nullptr) {
        g_trayIcon->Remove();
      }
      if (g_mainWindow != nullptr) {
        g_mainWindow->Hide();
      }
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wParam, lParam);
  }
}

// Whether the command line holds `arg` as a whole argument.
bool HasArg(const wchar_t* commandLine, std::wstring_view arg) {
  int count = 0;
  const wil::unique_hlocal_ptr<wchar_t*> args(CommandLineToArgvW(commandLine, &count));
  if (!args) {
    return false;
  }
  for (int i = 0; i < count; ++i) {
    if (arg == args.get()[i]) {
      return true;
    }
  }
  return false;
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
  // Sharp at any scaling: the window sizes itself for its monitor's DPI.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // The file dialog ("add exe") is COM, on this thread.
  const auto com = wil::CoInitializeEx(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  // The whole command line: CommandLineToArgvW expects argv[0] first.
  const bool background = HasArg(GetCommandLineW(), sovereign::tray::kBackgroundArg);

  // One tray per session: a second one would fight the first over the box.
  // Started by hand while one runs, it brings that one's window up instead.
  const wil::unique_mutex_nothrow single(CreateMutexW(nullptr, FALSE, L"Local\\SovereignTray"));
  if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND running = FindWindowW(kTrayClassName, nullptr); running != nullptr && !background) {
      DWORD pid = 0;
      GetWindowThreadProcessId(running, &pid);
      AllowSetForegroundWindow(pid);  // this process has the foreground right now; the window needs it
      PostMessageW(running, ShowWindowMessage(), 0, 0);
    }
    return 0;
  }
  sovereign::tray::RefreshAutostart();

  TraySettings settings;
  try {
    settings = sovereign::tray::LoadSettings();
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("reading tray settings failed; starting turned off");
  }

  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = kTrayClassName;
  if (!RegisterClassW(&windowClass)) {
    return 1;
  }
  // A message-only window can't receive broadcasts like TaskbarCreated, so
  // this one is a normal (never shown) top-level window.
  HWND window = CreateWindowExW(0, kTrayClassName, L"sovereign tray", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (!window) {
    return 1;
  }
  // The second start's message comes from another process of the same user
  // at the same integrity: let it through UIPI anyway, in case one runs elevated.
  ChangeWindowMessageFilterEx(window, ShowWindowMessage(), MSGFLT_ALLOW, nullptr);
  {
    const std::scoped_lock lock(State().mutex);
    State().window = window;
    State().view.wantOn = settings.wantOn;
  }

  const auto onCommand = [window](UiCommand command, const sovereign::tray::UiArgs& args) {
    OnUiCommand(window, command, args);
  };
  sovereign::tray::MainWindow mainWindow(instance, onCommand);
  sovereign::tray::Updater updater(
      sovereign::tray::ParseVersion(Narrow(SOVEREIGN_VERSION_W)).value_or(sovereign::tray::Version{}), kUserAgent,
      [window] { PostMessageW(window, kUpdateChangedMessage, 0, 0); });
  g_updater = &updater;
  g_mainWindow = &mainWindow;
  if (!background) {
    ShowMainWindow(UiPage::Overview);
  }

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
  g_updater = nullptr;
  g_mainWindow = nullptr;
  return exitCode;
}
