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
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <expected>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "app_rules.h"
#include "autostart.h"
#include "capture.h"
#include "combine.h"
#include "diagnose.h"
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
#include "profiles.h"
#include "relay.h"
#include "settings.h"
#include "share_links.h"
#include "sha256.h"
#include "subscription.h"
#include "support_report.h"
#include "tray_model.h"
#include "ui_content.h"
#include "update.h"
#include "updater.h"
#include "warp.h"
#include "x25519.h"

namespace {

using sovereign::tray::Action;
using sovereign::tray::AppsMode;
using sovereign::tray::Display;
using sovereign::tray::Profile;
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

// What every server the tray talks to sees - subscription panels first: the
// core and its version, as the official sing-box apps put it, and nothing of
// Sovereign's own. A client name that only one person uses would tie that
// person together across every panel; "sing-box 1.x.y" is what panels pick
// the format by (packetlab's: the plain one, no Mieru; the version for the
// DNS format). The version is the pinned core's.
constexpr wchar_t kUserAgent[] = L"sing-box " SOVEREIGN_SINGBOX_VERSION_W;

// A subscription not fetched so its server doesn't learn the user's address.
constexpr const char* kHeldBack =
    "не скачана: подключения нет, а напрямую её сервер увидел бы твой IP. Включи подключение, задай "
    "пересыльщик в настройках или выбери «Скачать напрямую» в меню конфигурации";

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
// Where each server is: lookups a box makes for one before giving up on it
// (QUIC-based servers drop a first connection now and then).
constexpr int kServerExitTries = 3;

// A latency test the service never finishes stops being waited for (a server
// takes up to 12 s, eight at a time).
constexpr std::chrono::seconds kUrlTestGiveUp{120};

// With an auto pick the servers are measured again this often (a test takes
// seconds and a few dozen small requests a server).
constexpr std::chrono::minutes kAutoRetest{10};

// A server that failed a test is tested again: soon after a first failure (to
// tell a hiccup from a server that's down - the main screen only says so
// after two in a row), and now and then while it keeps failing (so the
// screen says it once it's back).
constexpr std::chrono::seconds kFailRecheck{30};
constexpr std::chrono::minutes kFailRetest{5};

// A switch of the running selector the service refused is retried this often.
constexpr std::chrono::seconds kSelectRetry{5};

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
  // Before the worker's first poll nothing is known: "connecting", not "the
  // service isn't running" (TrayModel says that only after kServiceGrace).
  Display display = Display::Starting;
  bool wantOn = false;
  double down = 0;
  double up = 0;
  std::int64_t connections = 0;
  std::string error;
  bool hasConfig = false;          // the configurations that are on make a config
  std::string combineError;        // why they don't
  std::vector<std::string> combineNotes;  // what was left out of it
  sovereign::tray::RoutingSettings routing;  // as set (routing.h)
  std::size_t listsReady = 0;             // the routing's lists downloaded
  std::size_t listsNeeded = 0;
  std::int64_t listsUpdated = 0;          // unix seconds of the last one downloaded
  std::string listsError;
  // The checks' page (diagnose.h): written by the checks' thread, not the worker.
  std::array<sovereign::tray::CheckResult, sovereign::tray::kCheckCount> checks{};
  bool checksRunning = false;
  // The configurations (profiles.h), in the settings' order.
  struct ServerView {
    std::string tag;
    std::string label;  // "VLESS · REALITY"
    bool enabled = true;
  };
  struct ProfileView {
    std::string id;
    std::string name;
    std::string url;               // for "copy the link" - the UI thread's clipboard; empty: the user's own
    bool enabled = true;
    std::int64_t lastRefresh = 0;
    int serverHours = 0;           // the server's interval (Profile-Update-Interval, or the default)
    int userHours = 0;             // the user's; 0 = the server's
    bool autoUpdate = true;
    std::string error;             // its last refresh failed with this
    bool edited = false;           // config.json differs from the subscription it's based on
    bool waiting = false;          // a newer subscription waits for the user's choice
    std::string choiceError;       // why the last choice about it didn't go through
    std::vector<std::string> mergeNotes;  // where the last carry-over met changes on both sides
    std::vector<ServerView> servers;
    bool heldBack = false;         // not fetched so its server doesn't see the user's IP
    std::string via;               // the configuration its servers connect through; "" = directly
    std::string supportUrl;        // what its panel said (profiles.h)
    std::uint64_t trafficUsed = 0;
    std::uint64_t trafficTotal = 0;
    std::int64_t expire = 0;
  };
  std::vector<ProfileView> profiles;
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
  std::uint64_t delaysRound = 0;  // tests over so far: a new one is done once this moves on
  std::string delayError;
  int autoOption = -1;      // the protocol that is auto (a URL test), if any
  std::string autoServer;   // with an auto pick, the server it runs on; "" until measured
  std::string selectError;  // why the box couldn't be switched to the pick
  sovereign::tray::ExitIp exitIp;  // through the server in use; empty while off
  std::vector<sovereign::tray::ExitIp> locations;  // per protocol: where it is, once known
  bool hideExitIp = false;
  std::string logLevel;
  bool killSwitch = false;
  bool killSwitchLan = true;
  bool killSwitchActive = false;
  std::string killSwitchError;
  std::string relayUrl;  // the subscription relay's address; never its key
  std::map<std::string, std::string> serviceVia;  // a service through a country: its server now ("" none there)
  bool relayKeySet = false;
};

// What to do with the config when a newer subscription meets the user's
// edits (config_sync.h), or to drop the edits.
enum class ConfigChoice : std::uint8_t {
  TakeNew,   // the new subscription replaces the config; the edits go to history
  KeepMine,  // the config stays; the new subscription becomes its base
  CarryOver, // the edits, merged into the new subscription
  Revert,    // back to the subscription as it arrived (the newest, if one waits)
};

// A change to one configuration from the window.
struct ProfileChange {
  enum class What : std::uint8_t {
    Enabled,     // on: its servers are in the config
    Name,        // text: the new name
    AutoUpdate,  // on
    Hours,       // hours: the refresh interval; 0 = the server's
    Server,      // text: a server's tag, on
    Via,         // text: the configuration to connect through; "" = directly
  };
  std::string id;
  What what = What::Enabled;
  bool on = false;
  std::string text;
  int hours = 0;
};

// A change to the routing from its page (routing.h).
struct RoutingChange {
  enum class What : std::uint8_t {
    RussiaDirect,   // on
    BlockAds,       // on
    BlockQuic,      // on
    FinalDirect,    // on
    Ipv4Only,       // on
    RemoteDns,      // value: RoutingSettings::RemoteDns
    LocalDns,       // value: RoutingSettings::LocalDns
    AddRule,        // text: what was typed; value: RouteRule::Action
    SetRuleAction,  // index; value: RouteRule::Action
    RemoveRule,     // index
    ImportFrom,     // text: a configuration's id - its rules copied
    Warp,           // on (a device registered first if there's none)
    AddService,     // text: the service's id - through WARP (WARP turned on)
    RemoveService,  // index
    SetServiceWay,  // index; text: "warp" (WARP turned on) or a country code
    WarpVia,        // on: over the proxy; off: directly
  };
  What what = What::RussiaDirect;
  bool on = false;
  int value = 0;
  int index = 0;
  std::string text;
};

// A config of the user's own to add as a configuration, and what to call it.
struct LocalImport {
  std::string text;
  std::string name;  // UTF-8: what it was made from ("Ключи: NL-1", a file's name)
};

// A new per-app setup from the window.
// The subscription relay's settings, each part on its own: the address
// ("" = no relay, the key goes too) or the key; nullopt = unchanged.
struct RelayChange {
  std::optional<std::string> url;
  std::optional<std::string> key;
};

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
  std::optional<std::string> pendingImport;  // a subscription URL to add (UTF-8)
  std::optional<LocalImport> pendingLocalConfig;  // a config of the user's own to add (a file, the clipboard)
  std::vector<ProfileChange> pendingChanges;   // in the order they were made
  std::vector<RoutingChange> pendingRouting;   // likewise
  std::optional<std::string> pendingRemove;    // a profile id to remove
  std::optional<std::string> pendingRefresh;   // a profile id to refresh now
  bool pendingRefreshDirect = false;           // ...directly: the user said so, its server sees their IP
  std::optional<RelayChange> pendingRelay;
  bool pendingUrlTest = false;
  bool pendingToggleExitIp = false;
  std::optional<std::string> pendingLogLevel;  // "" = the config's
  bool pendingToggleKillSwitch = false;
  bool pendingToggleKillSwitchLan = false;
  std::optional<AppsChange> pendingApps;
  std::optional<std::string> pendingProtocol;  // a selector option; "" = the config's own default
  std::optional<std::pair<std::string, ConfigChoice>> pendingChoice;  // a profile id and what to do
  View view;
  HWND window = nullptr;
  // The log window's lines, newest last, and how many were ever added (the
  // UI shows what came after the count it last saw).
  std::deque<std::wstring> logs;
  std::uint64_t logsAdded = 0;

  bool HasRequests() const {
    return pendingWant || pendingImport || pendingLocalConfig || !pendingChanges.empty() || !pendingRouting.empty() ||
           pendingRemove ||
           pendingRefresh || pendingRelay ||
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
    return "нет конфига: добавь или включи конфигурацию (окно → Конфигурации)";
  }
  auto parsed = nlohmann::json::parse(*config, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return "config.json: не JSON";
  }
  if (sovereign::tray::ConfigHasTun(*config)) {
    if (const auto other = ForeignSingTun()) {
      return "уже работает другой клиент sing-box с TUN (адаптер " + Narrow(*other) +
             ") — выключи его, Sovereign подключится сам";
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
    for (const Profile& profile : settings_.profiles) {
      AdoptOriginal(profile);
    }
    while (!stop.stop_requested()) {
      std::optional<bool> want;
      std::optional<std::string> import;
      std::optional<AppsChange> apps;
      std::optional<std::string> protocol;
      std::optional<std::pair<std::string, ConfigChoice>> choice;
      std::optional<LocalImport> local;
      std::vector<ProfileChange> changes;
      std::vector<RoutingChange> routing;
      std::optional<std::string> remove;
      std::optional<std::string> refresh;
      bool refreshDirect = false;
      std::optional<RelayChange> relay;
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
        changes = std::exchange(shared.pendingChanges, {});
        routing = std::exchange(shared.pendingRouting, {});
        remove = std::exchange(shared.pendingRemove, std::nullopt);
        refresh = std::exchange(shared.pendingRefresh, std::nullopt);
        refreshDirect = std::exchange(shared.pendingRefreshDirect, false);
        relay = std::exchange(shared.pendingRelay, std::nullopt);
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
        AddLocal(local->text, local->name);
      }
      for (const ProfileChange& change : changes) {
        Apply(change);
      }
      for (const RoutingChange& change : routing) {
        ChangeRouting(change);
      }
      UpdateRuleSets();
      if (remove) {
        RemoveProfile(*remove);
      }
      if (import) {
        AddSubscription(*import);
      }
      if (relay) {
        if (relay->url) {
          settings_.relayUrl = std::move(*relay->url);
          if (settings_.relayUrl.empty()) {
            settings_.relayKey.clear();  // no address: no relay at all
          }
        }
        if (relay->key) {
          settings_.relayKey = std::move(*relay->key);
        }
        Save();
      }
      if (refresh) {
        Refresh(*refresh, refreshDirect ? RefreshReason::Direct : RefreshReason::Asked);
      }
      RefreshDue();
      if (choice) {
        Choose(choice->first, choice->second);
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
        Save();  // the running box switches to it (ApplySelection), no restart
        if (autoPick_.server.empty()) {
          PickAutoServer();  // auto now: from the last test, not sing-box's until the next one
        }
      }
      const auto config = EffectiveConfig();
      // Nothing left to run - every configuration off, every server off: the
      // box stops rather than go on with a config that isn't wanted any more.
      if (!config && model_.WantOn()) {
        settings_.wantOn = false;
        Save();
        Execute(model_, model_.SetWantOn(false, TrayModel::Clock::now()), std::nullopt);
        Notify(L"Sovereign: подключение выключено", L"Нечего запускать: " + Widen(combineError_), true,
               UiPage::Subscription);
      }
      const std::string expected = ExpectedConfigHash(config);
      model_.SetExpectedConfig(expected);
      const auto stats = PollStats();
      const Action action = model_.OnPoll(stats, TrayModel::Clock::now());
      Execute(model_, action, config);
      // The box runs this very config: its subscription hosts go through the proxy.
      if (stats && stats->running && !expected.empty() && stats->configSha256 == expected) {
        runningHosts_ = combinedHosts_;
      } else if (!stats || !stats->running) {
        runningHosts_.clear();
      }
      UpdateDelays(stats, urlTest);
      ApplySelection(stats);
      ApplyServiceSelections(stats);
      UpdateExitIp(stats);
      UpdateServerExits(stats);
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
  // Why a subscription is fetched: what the user hears about it.
  enum class RefreshReason : std::uint8_t {
    Due,    // its interval passed: a balloon if it failed and it's on
    Asked,  // "Refresh": always a balloon
    Added,  // just added: the caller speaks
    Direct,  // "download directly": with no connection, the user lets its server see their IP
  };

  // What the worker knows about a configuration besides its settings.
  struct ProfileRuntime {
    std::string error;  // the last refresh failed with this
    std::optional<TrayModel::Clock::time_point> lastFailure;
    bool heldBack = false;  // not fetched at all: no connection, no relay - its server would see the user's IP
    std::string choiceError;              // why the last choice about a waiting version didn't go through
    std::vector<std::string> mergeNotes;  // where the last carry-over met changes on both sides
    std::optional<std::string> seenOriginal;  // the texts below were computed from
    std::optional<std::string> seenConfig;
    bool edited = false;   // config.json differs from the subscription it's based on
    bool waiting = false;  // a newer subscription waits for the user's choice
    std::vector<sovereign::tray::ServerInfo> servers;  // in config.json
  };

  // A configuration's folder; nullopt if it can't be made (the user's
  // profile folder is gone or read-only) - WIL has reported why.
  static std::optional<std::filesystem::path> DirOf(const std::string& id) {
    try {
      return sovereign::tray::ProfileDir(id);
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("the profile's folder can't be used");
      return std::nullopt;
    }
  }

  // The configurations that are on, together (combine.h): what the box runs
  // before the tray's own additions. combineError_ says why there's none.
  std::optional<std::string> Combined() {
    using sovereign::tray::RoutingSettings;
    std::vector<sovereign::tray::ProfileConfig> parts;
    std::vector<std::string> notes;
    for (const Profile& profile : settings_.profiles) {
      if (!profile.enabled) {
        continue;
      }
      const auto dir = DirOf(profile.id);
      const auto config = dir ? sovereign::tray::LoadConfig(*dir) : std::nullopt;
      if (!config) {
        notes.push_back(profile.name + ": ещё нет конфига");
        continue;
      }
      parts.push_back(
          {.name = profile.name, .config = *config, .disabled = profile.disabled, .id = profile.id, .via = profile.via});
    }
    // The frame is always the client's own: from the configurations only their
    // servers - their routing and DNS never (one client's routing for everyone,
    // services like Netflix included).
    RoutingSettings routing = settings_.routing;
    auto combined = sovereign::tray::CombineConfigs(parts, sovereign::tray::OwnFrame(routing));
    combineError_ = combined.error;
    notes.insert(notes.end(), combined.notes.begin(), combined.notes.end());
    combineNotes_ = std::move(notes);
    if (combined.config) {
      // The subscription servers through the proxy, ahead of every rule - a
      // Russian one too, and one the user sends directly: a panel sees the
      // proxy's address, not the user's. (With the box off a fetch goes
      // directly; nothing to route it through then.)
      const auto hosts = sovereign::tray::SubscriptionHosts(settings_.profiles);
      combinedHosts_ = hosts;
      std::string list;
      for (const std::string& host : hosts) {
        list += (list.empty() ? "" : ", ") + host;
      }
      if (auto viaProxy = sovereign::tray::ParseRule(list, sovereign::tray::RouteRule::Action::Proxy)) {
        routing.rules.insert(routing.rules.begin(), std::move(*viaProxy));
      }
      // The client's rules first in whatever runs (routing.h).
      combined.config = sovereign::tray::ApplyRouting(*combined.config, routing, ruleFiles_);
    }
    combined_ = combined.config;
    return std::move(combined.config);
  }

  // The routing's lists: downloaded when missing or a day old, checked every
  // ten minutes (at once after the routing changed), one download a poll so
  // the window stays answered. The box gets them as local files: a list that
  // can't be fetched never keeps it from starting.
  void UpdateRuleSets() {
    const auto now = TrayModel::Clock::now();
    if (lastRuleCheck_ && now - *lastRuleCheck_ < std::chrono::minutes(10) && !rulesDirty_) {
      return;
    }
    rulesDirty_ = false;
    std::map<std::string, std::string> files;
    bool downloaded = false;
    bool pending = false;
    for (const auto& source : sovereign::tray::NeededRuleSets(settings_.routing)) {
      try {
        const auto age = sovereign::tray::RuleSetAge(source.tag);
        if ((!age || *age >= std::chrono::hours(24)) && !downloaded) {
          downloaded = true;  // one a poll
          auto got = sovereign::tray::Download(Widen(source.url), kUserAgent, std::size_t{16} * 1024 * 1024);
          if (got && sovereign::tray::IsRuleSetFile(*got)) {
            sovereign::tray::SaveRuleSet(source.tag, *got);
            rulesError_.clear();
            rulesUpdated_ = std::time(nullptr);
          } else {
            rulesError_ = got ? std::string("ответ — не список правил") : got.error();
          }
        } else if (!age || *age >= std::chrono::hours(24)) {
          pending = true;
        }
        if (sovereign::tray::RuleSetAge(source.tag)) {
          files[source.tag] = Narrow(sovereign::tray::RuleSetPath(source.tag).wstring());
        }
      } catch (...) {
        LOG_CAUGHT_EXCEPTION_MSG("the routing's lists folder can't be used");
      }
    }
    ruleFiles_ = std::move(files);
    // More to fetch: the next poll goes on; all there: ten minutes' rest.
    if (pending) {
      rulesDirty_ = true;
    } else {
      lastRuleCheck_ = now;
    }
  }

  // A change to the routing from its page.
  void ChangeRouting(const RoutingChange& change) {
    using sovereign::tray::RoutingSettings;
    RoutingSettings& r = settings_.routing;
    switch (change.what) {
      case RoutingChange::What::RussiaDirect: r.russiaDirect = change.on; break;
      case RoutingChange::What::BlockAds: r.blockAds = change.on; break;
      case RoutingChange::What::BlockQuic: r.blockQuic = change.on; break;
      case RoutingChange::What::FinalDirect: r.finalDirect = change.on; break;
      case RoutingChange::What::Ipv4Only: r.ipv4Only = change.on; break;
      case RoutingChange::What::Warp:
        if (change.on && !r.warpAccount) {
          // The first time: a device of its own at Cloudflare.
          auto account = RegisterWarp();
          if (!account) {
            Notify(L"Sovereign: WARP не включился", Widen(account.error()), true, UiPage::Routing);
            break;
          }
          r.warpAccount = std::move(*account);
        }
        r.warp = change.on;
        break;
      case RoutingChange::What::AddService: {
        const bool known = sovereign::tray::FindService(change.text) != nullptr;
        const bool twice = std::any_of(r.services.begin(), r.services.end(),
                                       [&](const sovereign::tray::ServiceRoute& s) { return s.id == change.text; });
        if (!known || twice) {
          break;
        }
        // Through WARP: WARP on - a device registered first, if there's none.
        if (!r.warpAccount) {
          auto account = RegisterWarp();
          if (!account) {
            Notify(L"Sovereign: WARP не включился", Widen(account.error()), true, UiPage::Routing);
            break;
          }
          r.warpAccount = std::move(*account);
        }
        r.warp = true;
        r.services.push_back({.id = change.text, .way = sovereign::tray::ServiceRoute::Way::Warp, .country = {}});
        break;
      }
      case RoutingChange::What::RemoveService:
        if (change.index >= 0 && static_cast<std::size_t>(change.index) < r.services.size()) {
          r.services.erase(r.services.begin() + change.index);
        }
        break;
      case RoutingChange::What::SetServiceWay: {
        if (change.index < 0 || static_cast<std::size_t>(change.index) >= r.services.size()) {
          break;
        }
        sovereign::tray::ServiceRoute& s = r.services[static_cast<std::size_t>(change.index)];
        if (sovereign::tray::IsCountryCode(change.text)) {
          s.way = sovereign::tray::ServiceRoute::Way::Country;
          s.country = change.text;
          break;
        }
        if (!r.warpAccount) {  // through WARP: WARP on - a device registered first, if there's none
          auto account = RegisterWarp();
          if (!account) {
            Notify(L"Sovereign: WARP не включился", Widen(account.error()), true, UiPage::Routing);
            break;
          }
          r.warpAccount = std::move(*account);
        }
        r.warp = true;
        s.way = sovereign::tray::ServiceRoute::Way::Warp;
        s.country.clear();
        break;
      }
      case RoutingChange::What::WarpVia: r.warpViaProxy = change.on; break;
      case RoutingChange::What::RemoteDns:
        r.remoteDns = static_cast<RoutingSettings::RemoteDns>(std::clamp(change.value, 0, 2));
        break;
      case RoutingChange::What::LocalDns:
        r.localDns = static_cast<RoutingSettings::LocalDns>(std::clamp(change.value, 0, 2));
        break;
      case RoutingChange::What::AddRule:
        if (auto rule = sovereign::tray::ParseRule(change.text, static_cast<sovereign::tray::RouteRule::Action>(
                                                                     std::clamp(change.value, 0, 3)))) {
          sovereign::tray::MergeRules(r.rules, {*rule});
        }
        break;
      case RoutingChange::What::SetRuleAction:
        if (change.index >= 0 && static_cast<std::size_t>(change.index) < r.rules.size()) {
          r.rules[static_cast<std::size_t>(change.index)].action =
              static_cast<sovereign::tray::RouteRule::Action>(std::clamp(change.value, 0, 3));
        }
        break;
      case RoutingChange::What::RemoveRule:
        if (change.index >= 0 && static_cast<std::size_t>(change.index) < r.rules.size()) {
          r.rules.erase(r.rules.begin() + change.index);
        }
        break;
      case RoutingChange::What::ImportFrom:
        if (const auto dir = sovereign::tray::FindProfile(settings_.profiles, change.text) != nullptr
                                 ? DirOf(change.text)
                                 : std::nullopt) {
          if (const auto config = sovereign::tray::LoadConfig(*dir)) {
            const std::size_t before = r.rules.size();
            sovereign::tray::MergeRules(r.rules, sovereign::tray::ImportRules(*config));
            Notify(L"Sovereign: правила взяты", std::format(L"новых правил: {}", r.rules.size() - before), false,
                   UiPage::Routing);
          }
        }
        break;
    }
    rulesDirty_ = true;  // a list may be wanted now
    Save();
  }

  // The combined config with the per-app rules, the log level and the cache
  // file applied - what the box must run. Not the protocol pick: that goes to
  // the running box (ApplySelection), a new pick needs no restart.
  std::optional<std::string> EffectiveConfig() {
    const auto config = Combined();
    if (!config) {
      return std::nullopt;
    }
    return sovereign::tray::EffectiveConfig(*config, {.appsMode = settings_.appsMode,
                                                      .apps = settings_.apps,
                                                      .logLevel = settings_.logLevel,
                                                      .cacheFile = cacheFile_});
  }

  // The selector's options in the combined config and which one the box
  // uses: the user's pick if it's still there, else the config's default.
  std::pair<std::vector<std::string>, int> Protocols() const {
    auto [choices, current] = Choices();
    return {std::move(choices.options), current};
  }

  // What the selector should be on: the pick; for an auto pick the server the
  // tray's measurements chose for it (auto itself - sing-box's own test,
  // one cold request a server - until there is one).
  std::string Wanted(const sovereign::tray::ProtocolChoices& choices, int current) const {
    if (current < 0) {
      return {};
    }
    const std::string& pick = choices.options[static_cast<std::size_t>(current)];
    const auto group = choices.groups.find(pick);
    if (group != choices.groups.end() &&
        std::find(group->second.begin(), group->second.end(), autoPick_.server) != group->second.end()) {
      return autoPick_.server;
    }
    return pick;
  }

  bool IsAuto(const sovereign::tray::ProtocolChoices& choices, int current) const {
    return current >= 0 && choices.groups.contains(choices.options[static_cast<std::size_t>(current)]);
  }

  std::pair<sovereign::tray::ProtocolChoices, int> Choices() const {
    if (!combined_) {
      return {{}, -1};
    }
    auto choices = sovereign::tray::FindProtocolChoices(*combined_);
    const auto find = [&](const std::string& tag) {
      const auto it = std::find(choices.options.begin(), choices.options.end(), tag);
      return it == choices.options.end() ? -1 : static_cast<int>(it - choices.options.begin());
    };
    int current = settings_.protocol.empty() ? -1 : find(settings_.protocol);
    if (current < 0) {
      current = find(choices.configDefault);
    }
    return {std::move(choices), current};
  }

  void Save() {
    try {
      sovereign::tray::SaveSettings(settings_);
    } catch (...) {
      // Still applies for this session; WIL has reported the cause.
      LOG_CAUGHT_EXCEPTION_MSG("saving tray.json failed");
    }
  }

  // Every subscription with auto-update on whose interval has passed, each
  // on its own schedule.
  void RefreshDue() {
    std::vector<std::string> due;
    for (const Profile& profile : settings_.profiles) {
      if (profile.url.empty() || !profile.autoUpdate) {
        continue;
      }
      const auto& failure = runtime_[profile.id].lastFailure;
      if (failure && TrayModel::Clock::now() - *failure < kRefreshRetry) {
        continue;
      }
      if (std::time(nullptr) - profile.lastRefresh >= std::int64_t{sovereign::tray::RefreshHours(profile)} * 3600) {
        due.push_back(profile.id);
      }
    }
    for (const std::string& id : due) {  // by id: a refresh may rename, not reorder
      Refresh(id, RefreshReason::Due);
    }
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
  static void AdoptOriginal(const Profile& profile) {
    if (profile.url.empty()) {
      return;
    }
    const auto dir = DirOf(profile.id);
    if (!dir) {
      return;
    }
    try {
      if (!sovereign::tray::LoadOriginal(*dir)) {
        if (const auto config = sovereign::tray::LoadConfig(*dir)) {
          sovereign::tray::SaveOriginal(*dir, *config);
        }
      }
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("saving subscription.json failed");
    }
  }

  // config.json becomes `text`; the one it replaces goes to history.
  static void ReplaceConfig(const std::filesystem::path& dir, const std::optional<std::string>& old,
                            const std::string& text) {
    if (old && !sovereign::tray::SameConfig(*old, text)) {
      sovereign::tray::SaveHistory(dir, *old);
    }
    sovereign::tray::SaveConfig(dir, text);
  }

  // A configuration's x-hwid (profiles.h): 128 random bits, nothing of the
  // machine's. std::random_device is the system's CSPRNG with MSVC (rand_s).
  static std::string NewHwid() {
    std::random_device random;
    std::string hwid;
    for (int i = 0; i < 4; ++i) {
      hwid += std::format("{:08x}", random());
    }
    return hwid;
  }

  // A WARP device of the user's own (warp.h): a WireGuard key made here, its
  // public half registered with Cloudflare as the official client does, the
  // device then switched on. The request goes the way the tray's traffic
  // goes - through the proxy while the box runs.
  static std::expected<sovereign::tray::WarpAccount, std::string> RegisterWarp() {
    std::random_device random;  // the system's CSPRNG with MSVC (rand_s)
    sovereign::tray::X25519Key privateKey{};
    for (std::size_t i = 0; i < privateKey.size(); i += 4) {
      const unsigned int word = random();
      for (std::size_t b = 0; b < 4; ++b) {
        privateKey[i + b] = static_cast<std::uint8_t>(word >> (8 * b));
      }
    }
    const auto bytes = [](const sovereign::tray::X25519Key& key) {
      return std::string(reinterpret_cast<const char*>(key.data()), key.size());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    };
    const std::string publicKey = sovereign::tray::EncodeBase64(bytes(sovereign::tray::X25519Public(privateKey)));
    SYSTEMTIME now{};
    GetSystemTime(&now);
    const std::string tos = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", now.wYear, now.wMonth, now.wDay,
                                        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    const std::wstring url = Widen(std::string(sovereign::tray::kWarpRegisterUrl));
    const std::wstring agent = Widen(std::string(sovereign::tray::kWarpUserAgent));
    const std::wstring headers = L"Content-Type: application/json; charset=UTF-8\r\nCF-Client-Version: " +
                                 Widen(std::string(sovereign::tray::kWarpClientVersion));
    const auto answer = sovereign::tray::HttpsSend(url, agent, L"POST",
                                                   sovereign::tray::WarpRegisterBody(publicKey, tos), headers, std::size_t{64} * 1024);
    if (!answer) {
      return std::unexpected("Cloudflare: " + answer.error());
    }
    auto account = sovereign::tray::ParseWarpRegistration(*answer, sovereign::tray::EncodeBase64(bytes(privateKey)));
    if (!account) {
      return std::unexpected(std::string("Cloudflare ответил не так, как ждали"));
    }
    // Switched on, as the official client does next; a failure here isn't
    // fatal (a new device works without it too).
    if (!account->id.empty() && !account->token.empty()) {
      [[maybe_unused]] const auto enabled = sovereign::tray::HttpsSend(
          url + L"/" + Widen(account->id), agent, L"PATCH", R"({"warp_enabled":true})",
          headers + L"\r\nAuthorization: Bearer " + Widen(account->token), std::size_t{64} * 1024);
    }
    return std::move(*account);
  }

  // Fetches configuration `id`'s subscription into its folder. False, with
  // the reason in its runtime error, if nothing usable came.
  bool Refresh(const std::string& id, RefreshReason reason) {
    Profile* profile = sovereign::tray::FindProfile(settings_.profiles, id);
    const auto dir = profile != nullptr ? DirOf(id) : std::nullopt;
    if (profile == nullptr || profile->url.empty() || !dir) {
      return false;
    }
    ProfileRuntime& state = runtime_[id];
    if (!sovereign::tray::IsHwid(profile->hwid)) {
      profile->hwid = NewHwid();
      Save();  // the same device to the panel from now on, even if this fetch fails
    }
    // Who sees the user's address decides how it's fetched:
    //  * the box runs a config whose rules send this host through the proxy:
    //    straight from here - it goes through the proxy;
    //  * else through the relay, if there is one: its server sees Cloudflare;
    //  * else not at all - unless the user said "directly" for this one.
    const auto hosts = sovereign::tray::SubscriptionHosts({*profile});
    const bool viaProxy = model_.GetDisplay() == Display::On && !hosts.empty() &&
                          std::find(runningHosts_.begin(), runningHosts_.end(), hosts.front()) != runningHosts_.end();
    std::expected<sovereign::tray::FetchResult, std::string> fetched = std::unexpected(std::string());
    state.heldBack = false;
    if (viaProxy || reason == RefreshReason::Direct) {
      fetched = sovereign::tray::FetchSubscription(Widen(profile->url), kUserAgent, profile->hwid);
    } else if (sovereign::tray::IsRelayUrl(settings_.relayUrl) && sovereign::tray::IsRelayKey(settings_.relayKey)) {
      fetched = sovereign::tray::FetchSubscriptionViaRelay(settings_.relayUrl, settings_.relayKey, Widen(profile->url),
                                                          kUserAgent, profile->hwid);
    } else {
      state.heldBack = true;
      fetched = std::unexpected(std::string(kHeldBack));
    }
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
      if (!state.heldBack) {  // held back: tried again as soon as there's a way, no back-off
        state.lastFailure = TrayModel::Clock::now();
      }
      state.error = error;
      // Held back on its schedule: quietly - it's fetched once there's a way.
      const bool quiet = state.heldBack && reason == RefreshReason::Due;
      if (!quiet && (reason == RefreshReason::Asked || reason == RefreshReason::Direct ||
                     (reason == RefreshReason::Due && profile->enabled))) {
        Notify(L"Sovereign: «" + Widen(profile->name) + L"» не обновилась",
               Widen(error) + L" (работает прежний конфиг)", true, UiPage::Subscription);
      }
      return false;
    }
    state.lastFailure.reset();
    state.error.clear();

    // Taken whole if the config has no edits of its own; otherwise it waits
    // for the user (config_sync.h).
    using sovereign::tray::Arrival;
    Arrival arrival = Arrival::Unchanged;
    bool alreadyWaiting = false;
    try {
      const auto original = sovereign::tray::LoadOriginal(*dir);
      const auto config = sovereign::tray::LoadConfig(*dir);
      arrival = sovereign::tray::ClassifyArrival(original, config, fetched->body);
      switch (arrival) {
        case Arrival::Unchanged:
          if (!original) {
            sovereign::tray::SaveOriginal(*dir, fetched->body);
          }
          sovereign::tray::ClearPending(*dir);  // the subscription went back to what the config is based on
          break;
        case Arrival::Replace:
          ReplaceConfig(*dir, config, fetched->body);
          sovereign::tray::SaveOriginal(*dir, fetched->body);
          sovereign::tray::ClearPending(*dir);
          state.mergeNotes.clear();  // about a config that's gone
          break;
        case Arrival::Ask: {
          const auto waiting = sovereign::tray::LoadPending(*dir);
          alreadyWaiting = waiting && sovereign::tray::SameConfig(*waiting, fetched->body);
          if (!alreadyWaiting) {
            sovereign::tray::SavePending(*dir, fetched->body);
          }
          break;
        }
      }
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("saving the subscription failed");
      state.error = "не удалось сохранить конфиг в %LOCALAPPDATA%\\Sovereign";
      return false;
    }
    profile->lastRefresh = std::time(nullptr);
    profile->updateHours =
        static_cast<int>(fetched->updateInterval.value_or(sovereign::tray::kDefaultUpdateInterval).count());
    // Where its support is, the traffic and the paid period: as the panel says
    // now (a header it stopped sending is gone here too).
    profile->supportUrl = fetched->supportUrl.value_or(std::string());
    profile->webPageUrl = fetched->webPageUrl.value_or(std::string());
    const sovereign::tray::SubscriptionUsage usage = fetched->usage.value_or(sovereign::tray::SubscriptionUsage{});
    profile->trafficUsed = usage.upload + usage.download;
    profile->trafficTotal = usage.total;
    profile->expire = usage.expire;
    // The panel's own name for the subscription, when it sends one - until
    // the user names it themselves.
    if (fetched->title && !profile->userNamed && *fetched->title != profile->name) {
      std::vector<Profile> others;
      std::copy_if(settings_.profiles.begin(), settings_.profiles.end(), std::back_inserter(others),
                   [&](const Profile& p) { return p.id != id; });
      profile->name = sovereign::tray::UniqueProfileName(others, *fetched->title);
    }
    Save();
    if (reason == RefreshReason::Added) {
      return true;
    }
    if (arrival == Arrival::Ask) {
      if (!alreadyWaiting && (profile->enabled || reason == RefreshReason::Asked || reason == RefreshReason::Direct)) {
        Notify(L"Sovereign: новая версия «" + Widen(profile->name) + L"»",
               L"В конфиге есть твои правки, поэтому он не заменён. Нажми, чтобы выбрать, что оставить.", false,
               UiPage::Subscription);
      }
      return true;
    }
    if (reason == RefreshReason::Asked || reason == RefreshReason::Direct) {
      Notify(L"Sovereign: «" + Widen(profile->name) + L"» обновлена",
             std::format(L"выходов в конфиге: {}{}", check.outbounds,
                         arrival == Arrival::Replace ? L"" : L" (без изменений)"),
             false);
    }
    // A running box with the old config is restarted by the model: the hash
    // the service reports no longer matches (ExpectedConfigHash).
    return true;
  }

  // A subscription link: a new configuration next to the others, on - or,
  // when that link is already one, that one switched on and refreshed.
  void AddSubscription(const std::string& url) {
    if (Profile* known = sovereign::tray::FindProfile(settings_.profiles,
                                                     FindIdByUrl(url))) {
      known->enabled = true;
      const std::string id = known->id;
      Save();
      Refresh(id, RefreshReason::Asked);
      return;
    }
    Profile profile;
    profile.id = sovereign::tray::NewProfileId(settings_.profiles, std::time(nullptr));
    profile.url = url;
    profile.name = sovereign::tray::UniqueProfileName(settings_.profiles, sovereign::tray::DefaultProfileName(url));
    settings_.profiles.push_back(profile);
    if (!Refresh(profile.id, RefreshReason::Added)) {
      if (runtime_[profile.id].heldBack) {
        // Not fetched on purpose: kept, and fetched once there's a way (or
        // directly, if the user says so from its menu).
        Save();
        Notify(L"Sovereign: подписка добавлена, но не скачана",
               L"Подключения нет, а напрямую её сервер увидел бы твой IP. Скачается, как только будет подключение или "
               L"пересыльщик (Настройки); или «Скачать напрямую» в её меню.",
               true, UiPage::Subscription);
        return;
      }
      // Nothing came: no configuration to keep - the link was wrong, or the
      // server is down (then pasting it again later adds it).
      const std::string error = runtime_[profile.id].error;
      ForgetProfile(profile.id);
      Notify(L"Sovereign: подписка не добавлена", Widen(error), true, UiPage::Subscription);
      return;
    }
    const Profile* added = sovereign::tray::FindProfile(settings_.profiles, profile.id);
    Notify(L"Sovereign: подписка добавлена",
           std::format(L"«{}» включена вместе с остальными.", Widen(added->name)), false, UiPage::Subscription);
  }

  std::string FindIdByUrl(const std::string& url) const {
    const Profile* known = sovereign::tray::FindProfileByUrl(settings_.profiles, url);
    return known != nullptr ? known->id : std::string();
  }

  // A config of the user's own (keys, a file, a QR code): a configuration of
  // its own, on. `name` says what it was made from.
  void AddLocal(const std::string& text, const std::string& name) {
    const auto check = sovereign::tray::CheckSubscriptionConfig(text);
    if (!check.ok) {
      Notify(L"Sovereign: конфиг не загружен", Widen(check.error), true, UiPage::Subscription);
      return;
    }
    // The same config pasted again: that one, not a copy of it.
    for (Profile& existing : settings_.profiles) {
      const auto dir = existing.url.empty() ? DirOf(existing.id) : std::nullopt;
      const auto config = dir ? sovereign::tray::LoadConfig(*dir) : std::nullopt;
      if (config && sovereign::tray::SameConfig(*config, text)) {
        existing.enabled = true;
        Save();
        Notify(L"Sovereign: такая конфигурация уже есть", L"«" + Widen(existing.name) + L"» включена.", false,
               UiPage::Subscription);
        return;
      }
    }
    Profile profile;
    profile.id = sovereign::tray::NewProfileId(settings_.profiles, std::time(nullptr));
    std::string base = sovereign::tray::CleanProfileName(name);
    profile.name = sovereign::tray::UniqueProfileName(settings_.profiles, base.empty() ? "Мой конфиг" : base);
    const auto dir = DirOf(profile.id);
    bool saved = false;
    try {
      if (dir) {
        sovereign::tray::SaveConfig(*dir, text);
        saved = true;
      }
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("saving the local config failed");
    }
    if (!saved) {
      Notify(L"Sovereign: конфиг не загружен", L"не удалось записать config.json", true, UiPage::Subscription);
      return;
    }
    settings_.profiles.push_back(profile);
    Save();
    Notify(L"Sovereign: конфигурация добавлена",
           std::format(L"«{}», выходов: {} — включена вместе с остальными.", Widen(profile.name), check.outbounds),
           false, UiPage::Subscription);
  }

  // A change to one configuration from the window. A running box picks up
  // what changes the config (on/off, a server) through its hash.
  void Apply(const ProfileChange& change) {
    Profile* profile = sovereign::tray::FindProfile(settings_.profiles, change.id);
    if (profile == nullptr) {
      return;
    }
    switch (change.what) {
      case ProfileChange::What::Enabled:
        profile->enabled = change.on;
        delays_.clear();  // the server list changes
        break;
      case ProfileChange::What::Name: {
        const std::string clean = sovereign::tray::CleanProfileName(change.text);
        if (clean.empty()) {
          return;
        }
        std::vector<Profile> others;
        std::copy_if(settings_.profiles.begin(), settings_.profiles.end(), std::back_inserter(others),
                     [&](const Profile& p) { return p.id != change.id; });
        profile->name = sovereign::tray::UniqueProfileName(others, clean);
        profile->userNamed = true;
        break;
      }
      case ProfileChange::What::AutoUpdate:
        profile->autoUpdate = change.on;
        runtime_[change.id].lastFailure.reset();  // a due refresh goes now, not after the retry pause
        break;
      case ProfileChange::What::Hours:
        profile->userHours = std::clamp(change.hours, 0, sovereign::tray::kMaxRefreshHours);
        break;
      case ProfileChange::What::Server:
        sovereign::tray::SetServerEnabled(*profile, change.text, change.on);
        break;
      case ProfileChange::What::Via:
        if (change.text.empty() || (change.text != profile->id && sovereign::tray::IsProfileId(change.text))) {
          profile->via = change.text;
          delays_.clear();  // its servers measure through another path now
        }
        break;
    }
    Save();
  }

  // A configuration out of the list and its folder gone - its config.json
  // kept in the data folder's history\ first, in case it was a mistake.
  void RemoveProfile(const std::string& id) {
    if (sovereign::tray::FindProfile(settings_.profiles, id) == nullptr) {
      return;
    }
    try {
      if (const auto dir = DirOf(id)) {
        if (const auto config = sovereign::tray::LoadConfig(*dir)) {
          sovereign::tray::SaveHistory(sovereign::tray::DataDir(), *config);
        }
      }
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("keeping the removed configuration failed");
    }
    ForgetProfile(id);
    delays_.clear();
  }

  // Out of the list, its folder removed.
  void ForgetProfile(const std::string& id) {
    std::erase_if(settings_.profiles, [&](const Profile& p) { return p.id == id; });
    runtime_.erase(id);
    try {
      sovereign::tray::RemoveProfileDir(id);
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("removing the profile's folder failed");  // a leftover folder, nothing more
    }
    Save();
  }

  void Choose(const std::string& id, ConfigChoice choice) {
    const auto dir = sovereign::tray::FindProfile(settings_.profiles, id) != nullptr ? DirOf(id) : std::nullopt;
    if (!dir) {
      return;
    }
    ProfileRuntime& state = runtime_[id];
    state.choiceError.clear();
    try {
      const auto original = sovereign::tray::LoadOriginal(*dir);
      const auto config = sovereign::tray::LoadConfig(*dir);
      const auto waiting = sovereign::tray::LoadPending(*dir);
      switch (choice) {
        case ConfigChoice::TakeNew:
          if (waiting) {
            ReplaceConfig(*dir, config, *waiting);
            sovereign::tray::SaveOriginal(*dir, *waiting);
            sovereign::tray::ClearPending(*dir);
            state.mergeNotes.clear();
          }
          break;
        case ConfigChoice::KeepMine:
          if (waiting) {
            sovereign::tray::SaveOriginal(*dir, *waiting);
            sovereign::tray::ClearPending(*dir);
          }
          break;
        case ConfigChoice::CarryOver: {
          if (!waiting || !original || !config) {
            break;
          }
          const auto merged = sovereign::tray::MergeConfigs(*original, *config, *waiting);
          if (!merged) {
            state.choiceError = "не получилось: config.json или подписка — не JSON-объект";
            break;
          }
          const auto check = sovereign::tray::CheckSubscriptionConfig(merged->config);
          if (!check.ok) {
            state.choiceError = "после переноса правок конфиг не годится: " + check.error;
            break;
          }
          ReplaceConfig(*dir, config, merged->config);
          sovereign::tray::SaveOriginal(*dir, *waiting);
          sovereign::tray::ClearPending(*dir);
          state.mergeNotes = merged->conflicts;
          break;
        }
        case ConfigChoice::Revert: {
          const auto& to = waiting ? waiting : original;
          if (to) {
            ReplaceConfig(*dir, config, *to);
            sovereign::tray::SaveOriginal(*dir, *to);
            sovereign::tray::ClearPending(*dir);
            state.mergeNotes.clear();
          }
          break;
        }
      }
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("applying the config choice failed");
      state.choiceError = "не удалось записать конфиг в %LOCALAPPDATA%\\Sovereign";
    }
    // As with a refresh: a changed config restarts the box through its hash.
  }

  // Each configuration's state on disk - edits of its own, a version
  // waiting, its servers - re-read every poll (config.json may be edited by
  // hand), worked out again only when a file changed.
  void UpdateConfigState() {
    for (const Profile& profile : settings_.profiles) {
      ProfileRuntime& state = runtime_[profile.id];
      const auto dir = DirOf(profile.id);
      std::optional<std::string> original;
      std::optional<std::string> config;
      try {
        if (dir) {
          original = sovereign::tray::LoadOriginal(*dir);
          config = sovereign::tray::LoadConfig(*dir);
        }
        state.waiting = dir && sovereign::tray::LoadPending(*dir).has_value();
      } catch (...) {
        LOG_CAUGHT_EXCEPTION_MSG("reading the config failed");
        continue;
      }
      if (config != state.seenConfig) {
        state.servers = config ? sovereign::tray::ListServers(*config) : std::vector<sovereign::tray::ServerInfo>{};
      }
      if (original != state.seenOriginal || config != state.seenConfig) {
        state.edited = !profile.url.empty() && original && config && !sovereign::tray::SameConfig(*original, *config);
        state.seenOriginal = std::move(original);
        state.seenConfig = std::move(config);
      }
    }
  }

  // The servers' latency: tested once every time a box starts, whenever the
  // user asks, and with an auto pick every kAutoRetest; results polled while a test runs (the service only starts
  // one - it takes seconds, and the pipe answers one request at a time).
  void UpdateDelays(const std::optional<sovereign::tray::Stats>& stats, bool asked) {
    if (!stats) {
      generation_ = -1;  // the service is gone, and its results with it
    } else if (stats->generation != generation_) {
      generation_ = stats->generation;  // a new box: the old results are gone with the old one
      delays_.clear();
      failedInRow_.clear();
      recheckAt_.reset();
      delaysRound_ += testing_ ? 1 : 0;  // the test that ran is over
      testing_ = false;
    }
    const bool on = model_.GetDisplay() == Display::On;
    const auto [choices, current] = Choices();
    const auto now = TrayModel::Clock::now();
    const bool autoDue = IsAuto(choices, current) && !testing_ && now - testStarted_ >= kAutoRetest;
    const bool recheckDue = recheckAt_ && !testing_ && now >= *recheckAt_;
    if (on && (asked || autoDue || recheckDue || testedGeneration_ != generation_)) {
      testedGeneration_ = generation_;
      recheckAt_.reset();
      StartUrlTest();
    } else if (asked) {
      delayError_ = "подключение выключено";
      ++delaysRound_;  // asked, and over: nothing to wait for
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
    const auto [choices, current] = Choices();
    const std::string tag = Wanted(choices, current);
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

  // Where each server is: its exit looked up through it once a box (the core
  // runs a few at a time), polled until known. What's known stays across
  // boxes until a new answer - a server rarely moves.
  void UpdateServerExits(const std::optional<sovereign::tray::Stats>& stats) {
    if (!stats || !stats->running || model_.GetDisplay() != Display::On) {
      return;
    }
    const auto now = TrayModel::Clock::now();
    if (stats->generation != serverExitsGeneration_) {
      serverExitsGeneration_ = stats->generation;
      serverExitsTries_.clear();
    } else if (serverExitsPolled_ && now - *serverExitsPolled_ < kExitIpPoll) {
      return;
    }
    serverExitsPolled_ = now;
    const auto [choices, current] = Choices();
    for (const std::string& tag : choices.options) {
      if (choices.groups.contains(tag)) {
        continue;  // auto: it's wherever its server is
      }
      int& tries = serverExitsTries_[tag];
      sovereign::tray::ExitIp& known = serverExits_[tag];
      if (!known.pending && tries >= kServerExitTries) {
        continue;
      }
      const bool refresh = !known.pending || tries == 0;  // else: polling the lookup this box runs
      const auto response = sovereign::tray::RequestService(
          nlohmann::json{{"cmd", "box_exitip"}, {"tag", tag}, {"refresh", refresh}}.dump());
      const auto parsed = response ? sovereign::tray::ParseExitIpResponse(*response) : std::nullopt;
      if (!parsed) {
        continue;  // the service didn't answer: asked again at the next poll
      }
      tries += refresh ? 1 : 0;
      if (parsed->pending || parsed->ip.empty()) {
        known.pending = parsed->pending;  // a failure keeps what was known, and is tried again
      } else {
        known = *parsed;
        tries = kServerExitTries;  // known: done for this box
      }
    }
  }

  // The running box's selector switched to what it should be on (Wanted) -
  // after every start of a box too: sing-box starts it on what its cache
  // file kept, not necessarily what is wanted now.
  void ApplySelection(const std::optional<sovereign::tray::Stats>& stats) {
    if (!stats || !stats->running) {
      selectError_.clear();
      return;
    }
    const auto [choices, current] = Choices();
    const std::string want = Wanted(choices, current);
    if (choices.selector.empty() || want.empty() ||
        (want == selected_ && stats->generation == selectedGeneration_)) {
      return;
    }
    const auto now = TrayModel::Clock::now();
    if (!selectError_.empty() && lastSelectTry_ && now - *lastSelectTry_ < kSelectRetry) {
      return;
    }
    lastSelectTry_ = now;
    selectError_ = ServiceCall(
        nlohmann::json{{"cmd", "box_select"}, {"selector", choices.selector}, {"outbound", want}}, "box_selected");
    if (selectError_.empty()) {
      selected_ = want;
      selectedGeneration_ = stats->generation;
    }
  }

  // Services sent through a country (routing.h): each one's selector on the
  // fastest server measured in that country, on the main group while none is
  // - after every start of a box too, as ApplySelection. No restart: the
  // selectors are in the config from the start, only switched.
  void ApplyServiceSelections(const std::optional<sovereign::tray::Stats>& stats) {
    serviceVia_.clear();
    if (!stats || !stats->running) {
      serviceSelected_.clear();
      return;
    }
    const auto choices = Choices().first;
    std::vector<std::string> servers;
    std::copy_if(choices.options.begin(), choices.options.end(), std::back_inserter(servers),
                 [&](const std::string& tag) { return !choices.groups.contains(tag); });
    std::map<std::string, std::string> exits;
    for (const auto& [tag, exit] : serverExits_) {
      if (!exit.country.empty()) {
        exits[tag] = exit.country;
      }
    }
    std::map<std::string, int> delays;
    for (const auto& [tag, delay] : delays_) {
      if (delay.state == sovereign::tray::Delay::State::Ok) {
        delays[tag] = delay.ms;
      }
    }
    const auto now = TrayModel::Clock::now();
    for (const sovereign::tray::ServiceRoute& s : settings_.routing.services) {
      if (s.way != sovereign::tray::ServiceRoute::Way::Country || choices.selector.empty()) {
        continue;
      }
      const std::string server = sovereign::tray::PickCountryServer(s.country, servers, exits, delays);
      serviceVia_[s.id] = server;
      const std::string want = server.empty() ? choices.selector : server;  // none there: as usual
      const std::string selector = sovereign::tray::ServiceOutboundTag(s.id);
      const auto done = serviceSelected_.find(selector);
      if (done != serviceSelected_.end() && done->second == std::pair{want, stats->generation}) {
        continue;
      }
      auto& tried = serviceSelectTry_[selector];
      if (tried && now - *tried < kSelectRetry) {
        continue;
      }
      tried = now;
      if (ServiceCall(nlohmann::json{{"cmd", "box_select"}, {"selector", selector}, {"outbound", want}},
                      "box_selected")
              .empty()) {
        serviceSelected_[selector] = {want, stats->generation};
        tried.reset();
      }
    }
  }

  // Auto's server after a latency test (delays.h: JudgeAuto).
  void PickAutoServer() {
    const auto [choices, current] = Choices();
    if (IsAuto(choices, current)) {
      autoPick_ = sovereign::tray::JudgeAuto(std::move(autoPick_),
                                             choices.groups.at(choices.options[static_cast<std::size_t>(current)]),
                                             delays_);
    }
  }

  void StartUrlTest() {
    // The servers, not the groups: auto's own result would only repeat one of theirs.
    const auto choices = Choices().first;
    std::vector<std::string> tags;
    std::copy_if(choices.options.begin(), choices.options.end(), std::back_inserter(tags),
                 [&](const std::string& tag) { return !choices.groups.contains(tag); });
    if (tags.empty()) {
      ++delaysRound_;  // nothing to test: over at once
      return;
    }
    const std::string error = ServiceCall(nlohmann::json{{"cmd", "box_urltest"}, {"tags", tags}}, "box_urltest_started");
    delayError_ = error;
    if (!error.empty()) {
      ++delaysRound_;
      return;
    }
    testing_ = true;
    testStarted_ = TrayModel::Clock::now();
    for (const std::string& tag : tags) {
      delays_[tag] = sovereign::tray::Delay{};  // pending until the service says
    }
    testedTags_ = std::move(tags);
  }

  // A test over: each server's failures in a row counted, the next test
  // planned while any fails (kFailRecheck).
  void CountFailures() {
    bool first = false;
    bool failing = false;
    for (const std::string& tag : testedTags_) {
      const auto it = delays_.find(tag);
      if (it == delays_.end() || it->second.state == sovereign::tray::Delay::State::Pending) {
        continue;  // given up on: no answer either way
      }
      int& count = failedInRow_[tag];
      count = it->second.state == sovereign::tray::Delay::State::Failed ? count + 1 : 0;
      first = first || count == 1;
      failing = failing || count > 0;
    }
    if (failing) {
      recheckAt_ = TrayModel::Clock::now() + (first ? kFailRecheck : std::chrono::seconds(kFailRetest));
    }
    ++delaysRound_;
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
      CountFailures();
      PickAutoServer();
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
    auto [choices, current] = Choices();
    std::string autoServer;  // "" until measured
    if (IsAuto(choices, current)) {
      autoServer = Wanted(choices, current);
      if (autoServer == choices.options[static_cast<std::size_t>(current)]) {
        autoServer.clear();
      }
    }
    const auto autoOption = std::find_if(choices.options.begin(), choices.options.end(),
                                         [&](const std::string& tag) { return choices.groups.contains(tag); });
    UpdateConfigState();  // reads the files: outside the lock
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
      v.hasConfig = combined_.has_value();
      v.combineError = combineError_;
      v.combineNotes = combineNotes_;
      v.routing = settings_.routing;
      v.listsReady = ruleFiles_.size();
      v.listsNeeded = sovereign::tray::NeededRuleSets(settings_.routing).size();
      v.listsUpdated = rulesUpdated_;
      v.listsError = rulesError_;
      v.profiles.clear();
      for (const Profile& profile : settings_.profiles) {
        const ProfileRuntime& state = runtime_[profile.id];
        View::ProfileView shown{.id = profile.id,
                                .name = profile.name,
                                .url = profile.url,
                                .enabled = profile.enabled,
                                .lastRefresh = profile.lastRefresh,
                                .serverHours = profile.updateHours,
                                .userHours = profile.userHours,
                                .autoUpdate = profile.autoUpdate,
                                .error = state.error,
                                .edited = state.edited,
                                .waiting = state.waiting,
                                .choiceError = state.choiceError,
                                .mergeNotes = state.mergeNotes,
                                .servers = {},
                                .heldBack = state.heldBack,
                                .via = profile.via,
                                .supportUrl = profile.supportUrl,
                                .trafficUsed = profile.trafficUsed,
                                .trafficTotal = profile.trafficTotal,
                                .expire = profile.expire};
        for (const auto& server : state.servers) {
          shown.servers.push_back(
              {.tag = server.tag, .label = server.label, .enabled = sovereign::tray::IsServerEnabled(profile, server.tag)});
        }
        v.profiles.push_back(std::move(shown));
      }
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
      v.protocols = choices.options;
      v.protocol = current;
      v.autoOption = autoOption == choices.options.end() ? -1 : static_cast<int>(autoOption - choices.options.begin());
      v.autoServer = autoServer;
      v.selectError = selectError_;
      v.serviceVia = serviceVia_;
      v.delays.clear();
      for (const std::string& option : v.protocols) {
        // A group shows the latency of the server it runs on.
        const auto group = choices.groups.find(option);
        const std::string& tag = group != choices.groups.end() && !v.autoServer.empty() ? v.autoServer : option;
        const auto it = delays_.find(tag);
        if (it == delays_.end()) {
          v.delays.emplace_back();
          continue;
        }
        sovereign::tray::Delay delay = it->second;
        if (const auto failed = failedInRow_.find(tag); failed != failedInRow_.end()) {
          delay.failedInRow = failed->second;
        }
        v.delays.emplace_back(std::move(delay));
      }
      v.delaysTesting = testing_;
      v.delaysRound = delaysRound_;
      v.delayError = delayError_;
      v.exitIp = exit_;
      v.locations.clear();
      for (const std::string& option : v.protocols) {
        const auto it = serverExits_.find(option);
        v.locations.push_back(it == serverExits_.end() ? sovereign::tray::ExitIp{} : it->second);
      }
      v.hideExitIp = settings_.hideExitIp;
      v.logLevel = settings_.logLevel;
      v.killSwitch = settings_.killSwitch;
      v.killSwitchLan = settings_.killSwitchLan;
      v.relayUrl = settings_.relayUrl;
      v.relayKeySet = !settings_.relayKey.empty();
      v.killSwitchActive = killSwitchActive_;
      v.killSwitchError = killSwitchError_;
      window = shared.window;
    }
    PostMessageW(window, kViewChangedMessage, 0, 0);
  }

  TraySettings settings_;
  TrayModel model_;
  std::string cacheFile_;  // UTF-8; empty: no cache file
  std::map<std::string, ProfileRuntime> runtime_;  // by profile id
  unsigned noticeId_ = 0;
  std::wstring noticeTitle_;
  std::wstring noticeText_;
  bool noticeIsError_ = false;
  std::optional<UiPage> noticePage_;
  std::optional<TrayModel::Clock::time_point> lastPathLookup_;
  std::optional<std::string> combined_;  // the configurations that are on, together, with the routing
  std::map<std::string, std::string> ruleFiles_;  // the routing's lists there are: tag -> path (UTF-8)
  std::optional<TrayModel::Clock::time_point> lastRuleCheck_;
  bool rulesDirty_ = true;      // check the lists at the next poll
  std::string rulesError_;      // the last download failed with this
  std::int64_t rulesUpdated_ = 0;  // unix seconds of the last list downloaded
  std::string combineError_;             // why there's none
  std::vector<std::string> combineNotes_;
  std::string rejectedRouting_;
  std::string rejectedWhy_;
  // The subscription hosts the tray's rules send through the proxy: in the
  // config last combined, and in the one the box is confirmed to run (its
  // hash) - a subscription added since isn't covered by the running box yet.
  std::vector<std::string> combinedHosts_;
  std::vector<std::string> runningHosts_;
  std::uint64_t logSince_ = 0;  // box_logs cursor
  std::string loggedError_;     // the model's error last put into the log
  std::map<std::string, sovereign::tray::Delay> delays_;  // by outbound tag
  std::map<std::string, int> failedInRow_;                // by outbound tag: tests in a row it failed
  std::vector<std::string> testedTags_;                   // in the test that runs or ran last
  std::optional<TrayModel::Clock::time_point> recheckAt_;  // the next test while a server fails
  std::uint64_t delaysRound_ = 0;  // tests over (or that couldn't start), ever: what a report waits on
  bool testing_ = false;
  TrayModel::Clock::time_point testStarted_{};
  std::int64_t generation_ = -1;        // the box the results belong to
  std::int64_t testedGeneration_ = -1;  // the box last tested automatically
  std::string delayError_;
  sovereign::tray::AutoPick autoPick_;  // the server auto runs on
  std::string selected_;                // what the running selector was last switched to
  std::int64_t selectedGeneration_ = -1;  // in which box
  // The services' country selectors (ApplyServiceSelections): what each was
  // switched to in which box, when a failed switch was last tried, and the
  // server each service goes through now ("" none in its country) - shown.
  std::map<std::string, std::pair<std::string, std::int64_t>> serviceSelected_;
  std::map<std::string, std::optional<TrayModel::Clock::time_point>> serviceSelectTry_;
  std::map<std::string, std::string> serviceVia_;
  std::string selectError_;
  std::optional<TrayModel::Clock::time_point> lastSelectTry_;
  sovereign::tray::ExitIp exit_;
  std::string exitTag_;               // the server exit_ is about
  std::int64_t exitGeneration_ = -1;  // and the box
  std::optional<TrayModel::Clock::time_point> exitAsked_;
  std::map<std::string, sovereign::tray::ExitIp> serverExits_;  // where each server is, by tag
  std::map<std::string, int> serverExitsTries_;  // lookups started in this box, per tag
  std::int64_t serverExitsGeneration_ = -1;   // which box
  std::optional<TrayModel::Clock::time_point> serverExitsPolled_;
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

void RequestLocalConfig(std::string text, std::string name) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingLocalConfig = LocalImport{.text = std::move(text), .name = std::move(name)};
  }
  Wake();
}

// A configuration to use, or to remove, by its id (profiles.h).
void RequestProfileChange(ProfileChange change) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingChanges.push_back(std::move(change));
  }
  Wake();
}

void RequestRemoveProfile(std::string id) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingRemove = std::move(id);
  }
  Wake();
}

// A configuration made of servers is named after them: the first one's
// name, and how many more.
std::string ServersName(const sovereign::tray::ImportItems& items, std::size_t servers) {
  std::string first;
  if (!items.links.empty()) {
    first = sovereign::tray::ParseShareLink(items.links.front()).name;
  } else if (!items.outbounds.empty()) {
    first = sovereign::tray::Field<std::string>(
        nlohmann::json::parse(items.outbounds.front(), nullptr, /*allow_exceptions=*/false), "tag", {});
  }
  if (first.empty()) {
    first = "Ключи";
  }
  return servers > 1 ? std::format("{} и ещё {}", first, servers - 1) : first;
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
// `source` names the configuration when one is made (a file's name); empty:
// it's named after what's in it.
bool ImportText(const std::string& text, bool quiet = false, const std::string& source = {}) {
  if (text.size() > sovereign::tray::kMaxSubscriptionBytes) {
    ImportBalloon(L"Слишком большой текст: больше 4 МБ.");
    return true;
  }
  const auto items = sovereign::tray::RecognizeImport(text);
  if (items.json) {
    // The worker checks it, and says why not.
    RequestLocalConfig(*items.json, source.empty() ? std::string("Конфиг sing-box") : source);
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
    RequestLocalConfig(*built.config, source.empty() ? ServersName(items, built.servers) : source);
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
  return ImportText(text, quiet, Narrow(std::filesystem::path(path).stem().wstring()));
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

// What the clipboard holds that could become a configuration, described
// (share_links.h DescribeImport) - read again only when the clipboard
// changes, and only asked for while there's no configuration at all.
std::wstring ClipboardOffer() {
  static DWORD seen = 0;
  static std::wstring offer;
  const DWORD now = GetClipboardSequenceNumber();
  if (now == seen) {
    return offer;
  }
  seen = now;
  offer.clear();
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) {
    return offer;
  }
  std::wstring text;
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr) {
    if (const auto* locked = static_cast<const wchar_t*>(GlobalLock(data)); locked != nullptr) {
      text.assign(locked, wcsnlen(locked, std::size_t{1} << 20U));  // a megabyte of text is enough to look at
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  offer = Widen(sovereign::tray::DescribeImport(sovereign::tray::RecognizeImport(Narrow(text))));
  return offer;
}

// The countries the servers really come out in (their measured exits), each
// once, by name - what a service can be sent through.
std::vector<std::string> KnownExitCountries(const View& view) {
  std::vector<std::string> countries;
  for (const sovereign::tray::ExitIp& exit : view.locations) {
    if (sovereign::tray::IsCountryCode(exit.country) &&
        std::find(countries.begin(), countries.end(), exit.country) == countries.end()) {
      countries.push_back(exit.country);
    }
  }
  std::sort(countries.begin(), countries.end(), [](const std::string& a, const std::string& b) {
    return sovereign::tray::CountryName(a) < sovereign::tray::CountryName(b);
  });
  return countries;
}

// The user's time zone now, in minutes east of UTC (the report's dates).
int UtcOffsetMinutes() {
  TIME_ZONE_INFORMATION zone{};
  const DWORD kind = GetTimeZoneInformation(&zone);
  const LONG extra = kind == TIME_ZONE_ID_DAYLIGHT   ? zone.DaylightBias
                     : kind == TIME_ZONE_ID_STANDARD ? zone.StandardBias
                                                     : 0;
  return static_cast<int>(-(zone.Bias + extra));
}

// The report for a configuration's support (support_report.h): built from
// what the window already shows - the servers' measurements and exits, what
// the panel said; nothing of the user's - saved where the user picks (the desktop
// first), a short version copied for a chat, and the panel's support link
// opened if it gave one.
void WriteSupportReport(HWND owner, const View::ProfileView& profile, const View& view) {
  using sovereign::tray::ReportServer;
  sovereign::tray::ReportInput in;
  in.now = std::time(nullptr);
  in.subscriptionName = profile.name;
  in.subscriptionHost = Narrow(sovereign::tray::UrlHost(Widen(profile.url)));
  in.lastRefresh = profile.lastRefresh;
  in.refreshError = profile.error;
  in.trafficUsed = profile.trafficUsed;
  in.trafficTotal = profile.trafficTotal;
  in.expire = profile.expire;
  try {
    if (const auto config = sovereign::tray::LoadConfig(sovereign::tray::ProfileDir(profile.id))) {
      in.servers = sovereign::tray::ReportServers(*config);
    }
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("reading the configuration for the report failed");
  }
  // The measurements are by the running config's tags: a server's own, or
  // "<tag> · <configuration>" where two configurations named servers alike.
  for (ReportServer& s : in.servers) {
    for (std::size_t i = 0; i < view.protocols.size(); ++i) {
      if (!sovereign::tray::IsRenamedTag(view.protocols[i], s.name, profile.name)) {
        continue;
      }
      if (i < view.delays.size() && view.delays[i]) {
        const sovereign::tray::Delay& d = *view.delays[i];
        if (d.state == sovereign::tray::Delay::State::Ok) {
          s.state = ReportServer::State::Ok;
          s.delay = d.ms;
          s.jitter = d.jitter;
          s.loss = d.loss;
        } else if (d.state == sovereign::tray::Delay::State::Failed) {
          s.state = ReportServer::State::Failed;
          s.error = d.error;
          s.failedInRow = d.failedInRow;
        }
        in.connected = true;
      }
      if (i < view.locations.size()) {
        s.exitIp = view.locations[i].ip;
        s.exitCountry = view.locations[i].country;
        s.exitIsp = view.locations[i].isp;
      }
      break;
    }
  }
  // The user's own address (what Russian sites see) - only to cut it out.
  {
    static const std::regex kIp(R"(\d{1,3}(?:\.\d{1,3}){3})");
    std::smatch found;
    const std::string& seen = view.checks[static_cast<std::size_t>(sovereign::tray::CheckId::RussiaDirect)].summary;
    if (std::regex_search(seen, found, kIp)) {
      in.userIp = found.str();
    }
  }
  const sovereign::tray::SupportReport report = sovereign::tray::BuildSupportReport(in);

  // Where: the desktop first, the user picks.
  std::wstring path;
  try {
    const auto dialog = wil::CoCreateInstance<IFileSaveDialog>(CLSID_FileSaveDialog);
    const std::array<COMDLG_FILTERSPEC, 1> filters{{{L"Текст", L"*.txt"}}};
    THROW_IF_FAILED(dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data()));
    THROW_IF_FAILED(dialog->SetDefaultExtension(L"txt"));
    THROW_IF_FAILED(dialog->SetTitle(L"Отчёт для поддержки"));
    std::wstring name = L"Отчёт для поддержки — " + Widen(profile.name) + L".txt";
    std::replace_if(
        name.begin(), name.end(),
        [](wchar_t c) { return std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos; }, L'_');
    THROW_IF_FAILED(dialog->SetFileName(name.c_str()));
    wil::com_ptr<IShellItem> desktop;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&desktop)))) {
      dialog->SetFolder(desktop.get());
    }
    const HRESULT shownDialog = dialog->Show(owner);
    if (shownDialog == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
      return;
    }
    THROW_IF_FAILED(shownDialog);
    wil::com_ptr<IShellItem> item;
    THROW_IF_FAILED(dialog->GetResult(&item));
    wil::unique_cotaskmem_string chosen;
    THROW_IF_FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen));
    path = chosen.get();
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("the save dialog failed");
    return;
  }
  // UTF-8 with a BOM and CRLF: Notepad and any support desk read it as is.
  std::string text = "\xEF\xBB\xBF";
  for (const char c : report.full) {
    text += c == '\n' ? std::string("\r\n") : std::string(1, c);
  }
  std::ofstream(std::filesystem::path(path), std::ios::binary) << text;
  CopyText(owner, Widen(report.brief));
  if (!profile.supportUrl.empty()) {
    ShellExecuteW(nullptr, L"open", Widen(profile.supportUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  if (g_trayIcon != nullptr) {
    g_trayIcon->Balloon(L"Sovereign: отчёт сохранён",
                        profile.supportUrl.empty()
                            ? L"Краткая версия скопирована — вставь её в чат поддержки и приложи файл."
                            : L"Краткая версия скопирована, чат поддержки открыт — вставь её и приложи файл.",
                        NIIF_INFO);
  }
}

// A run of the checks (diagnose.h), on a thread of its own: they take
// seconds, the speed test half a minute. One at a time.
std::optional<std::jthread> g_checks;

// The newest log lines (the core's and the tray's), UTF-8: where "where does
// this go" reads the outbound.
std::vector<std::string> RecentLogLines(std::size_t count) {
  auto& shared = State();
  const std::scoped_lock lock(shared.mutex);
  std::vector<std::string> lines;
  const std::size_t from = shared.logs.size() > count ? shared.logs.size() - count : 0;
  for (std::size_t i = from; i < shared.logs.size(); ++i) {
    lines.push_back(Narrow(shared.logs[i]));
  }
  return lines;
}

void StartChecks(std::vector<sovereign::tray::CheckId> which, std::string host) {
  auto& shared = State();
  sovereign::tray::RoutingSettings routing;
  {
    const std::scoped_lock lock(shared.mutex);
    if (shared.view.checksRunning) {
      return;
    }
    shared.view.checksRunning = true;
    for (const auto id : which) {
      shared.view.checks[static_cast<std::size_t>(id)] = {};
    }
    routing = shared.view.routing;
  }
  g_checks.reset();  // the last run's thread, finished
  g_checks.emplace([which = std::move(which), host = std::move(host), routing](const std::stop_token& stop) {
    const auto post = [] {
      auto& s = State();
      HWND window = nullptr;
      {
        const std::scoped_lock lock(s.mutex);
        window = s.window;
      }
      PostMessageW(window, kViewChangedMessage, 0, 0);
    };
    const sovereign::tray::DiagnoseInput input{
        .routing = routing, .host = host, .userAgent = kUserAgent, .logs = [] { return RecentLogLines(400); }};
    sovereign::tray::RunChecks(stop, which, input,
                               [&](sovereign::tray::CheckId id, const sovereign::tray::CheckResult& result) {
                                 {
                                   auto& s = State();
                                   const std::scoped_lock lock(s.mutex);
                                   s.view.checks[static_cast<std::size_t>(id)] = result;
                                 }
                                 post();
                               });
    {
      auto& s = State();
      const std::scoped_lock lock(s.mutex);
      s.view.checksRunning = false;
    }
    post();
  });
}

void RequestRoutingChange(RoutingChange change) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingRouting.push_back(std::move(change));
  }
  Wake();
}

// A popup menu of `items` at `at`: the one picked (its index), or nullopt.
std::optional<std::size_t> PickFromMenu(HWND window, POINT at, const std::vector<std::wstring>& items, int checked) {
  wil::unique_hmenu menu(CreatePopupMenu());
  if (!menu) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    AppendMenuW(menu.get(), MF_STRING | (static_cast<int>(i) == checked ? MF_CHECKED : MF_UNCHECKED),
                static_cast<UINT>(i + 1), items[i].c_str());
  }
  SetForegroundWindow(window);
  const auto command = static_cast<UINT>(
      TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0, window,
                     nullptr));
  if (command == 0 || command > items.size()) {
    return std::nullopt;
  }
  return command - 1;
}

// A rule's action, as the menus and the page say it (RouteRule::Action's order).
const std::vector<std::wstring>& RuleActions() {
  static const std::vector<std::wstring> actions = {L"напрямую", L"через прокси", L"блокировать", L"через WARP"};
  return actions;
}

void RequestRefresh(std::string id, bool direct = false) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingRefresh = std::move(id);
    shared.pendingRefreshDirect = direct;
  }
  Wake();
}

void RequestRelay(RelayChange change) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingRelay = std::move(change);
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

void RequestChoice(std::string id, ConfigChoice choice) {
  {
    auto& shared = State();
    const std::scoped_lock lock(shared.mutex);
    shared.pendingChoice = std::pair(std::move(id), choice);
  }
  Wake();
}

// The refresh interval of a configuration from a menu: the server's, a few
// usual ones - the hours picked (0 = the server's) - or kCustomPeriod, for a
// number typed in place; nullopt when the menu was dismissed.
constexpr int kCustomPeriod = -1;
std::optional<int> ChooseRefreshPeriod(HWND window, POINT at, const View::ProfileView& profile) {
  wil::unique_hmenu menu(CreatePopupMenu());
  if (!menu) {
    return std::nullopt;
  }
  static constexpr std::array<int, 8> kHours = {0, 1, 3, 6, 12, 24, 48, 168};
  const auto name = [](int hours) {
    return hours % 24 == 0 && hours >= 24 ? std::format(L"{} сут", hours / 24) : std::format(L"{} ч", hours);
  };
  for (std::size_t i = 0; i < kHours.size(); ++i) {
    const int hours = kHours[i];
    const std::wstring label = hours == 0 ? L"Как советует сервер (" + name(profile.serverHours) + L")"
                                          : L"Каждые " + name(hours);
    AppendMenuW(menu.get(), MF_STRING | (profile.userHours == hours ? MF_CHECKED : MF_UNCHECKED),
                static_cast<UINT>(i + 1), label.c_str());
  }
  const bool custom = std::find(kHours.begin(), kHours.end(), profile.userHours) == kHours.end();
  constexpr UINT kOther = 100;
  const std::wstring other = custom ? L"Свой: каждые " + name(profile.userHours) + L"…" : std::wstring(L"Свой период…");
  AppendMenuW(menu.get(), MF_STRING | (custom ? MF_CHECKED : MF_UNCHECKED), kOther, other.c_str());
  SetForegroundWindow(window);
  const auto command = static_cast<UINT>(
      TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0, window,
                     nullptr));
  if (command >= 1 && command <= kHours.size()) {
    return kHours[command - 1];
  }
  if (command == kOther) {
    return kCustomPeriod;
  }
  return std::nullopt;
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

// A support report asked for while the box runs: written once a fresh test
// of the servers is over (View::delaysRound moves past `round`) - not from
// whatever the last test said, hours ago.
struct PendingReport {
  std::string profileId;
  HWND owner = nullptr;
  std::uint64_t round = 0;
};
std::optional<PendingReport> g_pendingReport;

// What the window shows, from the worker's view.
UiContent ContentFrom(const View& v) {
  UiContent c;
  if (v.profiles.empty()) {
    c.clipboardOffer = ClipboardOffer();
  }
  c.display = v.display;
  c.on = v.wantOn;
  c.error = v.display == Display::Error ? Widen(v.error) : std::wstring();
  c.hasConfig = v.hasConfig;
  c.combineError = Widen(v.combineError);
  for (const std::string& note : v.combineNotes) {
    c.combineNotes.push_back(Widen(note));
  }
  const auto hours = [](int h) {
    return h % 24 == 0 && h >= 24 ? std::format(L"{} сут", h / 24) : std::format(L"{} ч", h);
  };
  for (const View::ProfileView& profile : v.profiles) {
    sovereign::tray::UiProfile shown;
    shown.id = profile.id;
    shown.name = Widen(profile.name);
    shown.enabled = profile.enabled;
    shown.subscription = !profile.url.empty();
    shown.failed = !profile.error.empty();
    shown.host = sovereign::tray::UrlHost(Widen(profile.url));
    shown.updated = profile.lastRefresh ? LocalTime(profile.lastRefresh) : L"ещё не загружена";
    shown.autoUpdate = profile.autoUpdate;
    shown.period = profile.userHours > 0
                       ? L"каждые " + hours(profile.userHours)
                       : L"каждые " + hours(profile.serverHours) + L" (как советует сервер)";
    shown.error = Widen(profile.error);
    shown.edited = profile.edited;
    shown.waiting = profile.waiting;
    shown.choiceError = Widen(profile.choiceError);
    shown.heldBack = profile.heldBack;
    shown.usage = Widen(sovereign::tray::UsageLine(profile.trafficUsed, profile.trafficTotal, profile.expire,
                                                   std::time(nullptr), UtcOffsetMinutes()));
    shown.support = !profile.supportUrl.empty();
    if (!profile.via.empty()) {
      const auto through = std::find_if(v.profiles.begin(), v.profiles.end(),
                                        [&](const View::ProfileView& p) { return p.id == profile.via; });
      shown.via = through != v.profiles.end() ? Widen(through->name) : std::wstring(L"удалённая конфигурация");
      shown.viaBroken = through == v.profiles.end() || !through->enabled || !through->via.empty();
    }
    for (const std::string& note : profile.mergeNotes) {
      shown.mergeNotes.push_back(Widen(note));
    }
    std::size_t off = 0;
    for (const View::ServerView& server : profile.servers) {
      shown.servers.push_back({.name = Widen(server.tag), .label = Widen(server.label), .enabled = server.enabled});
      off += server.enabled ? 0 : 1;
    }
    // The list's line: what it is, how many servers, how fresh.
    std::wstring detail = shown.subscription ? L"подписка" : L"свой конфиг";
    detail += std::format(L" · серверов: {}", profile.servers.size() - off);
    if (off > 0) {
      detail += std::format(L" (выкл. {})", off);
    }
    if (!profile.enabled) {
      detail.insert(0, L"выключена · ");
    } else if (shown.waiting) {
      detail += L" · новая версия ждёт";
    } else if (shown.failed) {
      detail += L" · не обновилась";
    } else if (shown.subscription && profile.lastRefresh) {
      detail += L" · " + shown.updated;
    }
    shown.detail = std::move(detail);
    // In trouble: said on the main screen, the first subscription that is.
    // Its servers' measurements by the running config's tags (its own, or
    // "<tag> · <configuration>" where combine renamed a clash).
    if (!profile.url.empty() && profile.enabled && c.trouble.empty()) {
      std::size_t measured = 0;
      std::size_t failed = 0;
      for (const View::ServerView& server : profile.servers) {
        for (std::size_t i = 0; server.enabled && i < v.protocols.size() && i < v.delays.size(); ++i) {
          if (sovereign::tray::IsRenamedTag(v.protocols[i], server.tag, profile.name) && v.delays[i]) {
            // A first failure isn't counted either way until the recheck says.
            const sovereign::tray::Delay& d = *v.delays[i];
            const bool down = d.state == sovereign::tray::Delay::State::Failed && d.failedInRow >= 2;
            measured += d.state == sovereign::tray::Delay::State::Ok || down ? 1 : 0;
            failed += down ? 1 : 0;
            break;
          }
        }
      }
      const std::string trouble = sovereign::tray::SubscriptionTrouble(
          profile.servers.size(), measured, failed, !profile.error.empty(), profile.trafficUsed, profile.trafficTotal,
          profile.expire, std::time(nullptr));
      if (!trouble.empty()) {
        c.trouble = L"«" + Widen(profile.name) + L"»: " + Widen(trouble);
        c.troubleProfile = static_cast<int>(c.profiles.size());  // this one's index, before it's added
        c.troubleSupport = !profile.supportUrl.empty();
      }
    }
    c.profiles.push_back(std::move(shown));
  }
  static constexpr std::array<const wchar_t*, sovereign::tray::kCheckCount> kCheckTitles = {
      L"Выход через прокси", L"Российское напрямую", L"Утечка DNS",        L"WebRTC / UDP", L"IPv6",
      L"Локальная сеть",     L"Задержка DNS",        L"Скорость интернета", L"Куда пойдёт адрес"};
  // What each check does - its "?" on the page.
  static constexpr std::array<const wchar_t*, sovereign::tray::kCheckCount> kCheckHints = {
      L"Какой адрес и страну видят зарубежные сайты (Cloudflare). Должен быть адрес сервера, а не твой.",
      L"Какой адрес видят российские сайты (Яндекс). С «Российское напрямую» — твой собственный: сайты не "
      L"видят адрес сервера и не свяжут его с тобой.",
      L"Чьи DNS-серверы на самом деле узнают адреса сайтов, которые ты открываешь (bash.ws). Российский "
      L"резолвер провайдера — утечка: он видит, куда ты ходишь.",
      L"Какой адрес браузер может выдать через WebRTC — запросом к STUN-серверам Google и Cloudflare по UDP. "
      L"Должен совпадать с адресом прокси.",
      L"Есть ли IPv6 и идёт ли он через прокси: IPv6 мимо туннеля раскрывает твой адрес.",
      L"Доступен ли роутер — ping шлюза. С kill switch'ем локальная сеть открыта, только если это включено в "
      L"настройках.",
      L"Сколько занимает узнать адрес сайта: зарубежного (через прокси) и российского (напрямую).",
      L"Скорость скачивания и отдачи через текущий маршрут (speed.cloudflare.com). Гоняет десятки мегабайт — "
      L"поэтому своей кнопкой.",
      L"Введи сайт — трей откроет его и по журналу ядра покажет, через какой выход он пошёл и по какому "
      L"правилу."};
  for (std::size_t i = 0; i < sovereign::tray::kCheckCount; ++i) {
    const auto& result = v.checks[i];
    c.checks.push_back({.title = kCheckTitles[i],
                        .summary = Widen(result.summary),
                        .detail = Widen(result.detail),
                        .status = static_cast<sovereign::tray::UiCheck::Status>(result.status),
                        .hint = kCheckHints[i]});
  }
  c.checksRunning = v.checksRunning;
  using sovereign::tray::RoutingSettings;
  const RoutingSettings& r = v.routing;
  c.routing.russiaDirect = r.russiaDirect;
  c.routing.blockAds = r.blockAds;
  c.routing.blockQuic = r.blockQuic;
  c.routing.finalDirect = r.finalDirect;
  c.routing.ipv4Only = r.ipv4Only;
  c.routing.warp = r.warp;
  c.routing.warpRegistered = r.warpAccount.has_value();
  c.routing.warpViaProxy = r.warpViaProxy;
  c.routing.warpAddress = r.warpAccount ? Widen(r.warpAccount->address4) : std::wstring();
  c.routing.remoteDns = r.remoteDns == RoutingSettings::RemoteDns::Google  ? L"Google (DoH)"
                        : r.remoteDns == RoutingSettings::RemoteDns::Quad9 ? L"Quad9 (DoH)"
                                                                            : L"Cloudflare (DoH)";
  c.routing.localDns = r.localDns == RoutingSettings::LocalDns::Google   ? L"Google (DoH)"
                       : r.localDns == RoutingSettings::LocalDns::System ? L"системный"
                                                                          : L"Cloudflare (DoH)";
  for (const auto& rule : r.rules) {
    c.routing.rules.push_back(
        {.text = Widen(sovereign::tray::RuleText(rule)),
         .action = RuleActions()[static_cast<std::size_t>(rule.action)],
         .warp = rule.action == sovereign::tray::RouteRule::Action::Warp});
  }
  const std::vector<std::string> countries = KnownExitCountries(v);
  for (const auto& s : r.services) {
    const auto* info = sovereign::tray::FindService(s.id);
    std::wstring way = r.warp ? L"через WARP" : L"через WARP · WARP выключен";
    if (s.way == sovereign::tray::ServiceRoute::Way::Country) {
      way = L"через " + Widen(sovereign::tray::CountryName(s.country));
      const auto via = v.serviceVia.find(s.id);
      if (via != v.serviceVia.end() && !via->second.empty()) {
        way += L" · " + Widen(via->second);
      } else if (countries.empty()) {
        way += L" — страны серверов ещё узнаются";
      } else {
        way += L" — сервера в этой стране нет, идёт как обычно";
      }
    }
    c.routing.services.push_back({.name = info != nullptr ? Widen(std::string(info->name)) : Widen(s.id),
                                  .way = std::move(way)});
  }
  if (!v.listsError.empty() && v.listsReady < v.listsNeeded) {
    c.routing.lists = L"Списки правил не скачались: " + Widen(v.listsError) + L". Пока работают зоны .ru/.рф/.su.";
    c.routing.listsFailed = true;
  } else if (v.listsReady < v.listsNeeded) {
    c.routing.lists = std::format(L"Списки правил скачиваются: {} из {}.", v.listsReady, v.listsNeeded);
  } else if (v.listsNeeded > 0) {
    c.routing.lists = L"Списки правил на месте" +
                      (v.listsUpdated ? L", обновлены " + LocalTime(v.listsUpdated) : std::wstring()) +
                      L"; обновляются раз в сутки.";
  }  c.appsInclude = v.appsMode == AppsMode::Include;
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
      d.jitter = delay->jitter;
      d.loss = delay->loss;
      d.samples = delay->samples;
      d.connect = delay->connect;
      d.error = Widen(delay->error);
    }
    c.delays.push_back(d);
  }
  c.delaysTesting = v.delaysTesting;
  c.canTestDelays = v.display == Display::On && !v.protocols.empty();
  c.reportPending = g_pendingReport.has_value();
  c.delayError = Widen(v.delayError);
  c.autoOption = v.autoOption;
  c.autoServer = Widen(v.autoServer);
  c.selectError = Widen(v.selectError);
  for (const auto& location : v.locations) {
    c.locations.push_back({.country = Widen(location.country),
                           .isp = Widen(sovereign::tray::ShortIsp(location.isp)),
                           .ip = Widen(location.ip)});
  }
  c.exitIp = Widen(v.exitIp.ip);
  c.exitCountry = Widen(v.exitIp.country);
  c.exitCountryName = CountryName(c.exitCountry);
  c.exitPending = v.exitIp.pending;
  c.hideExitIp = v.hideExitIp;
  c.logLevel = Widen(v.logLevel);
  c.killSwitch = v.killSwitch;
  c.killSwitchLan = v.killSwitchLan;
  c.relayHost = v.relayUrl.empty() ? std::wstring() : sovereign::tray::UrlHost(Widen(v.relayUrl));
  c.relaySet = !v.relayUrl.empty() && v.relayKeySet;
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

// The report asked for, once its test is over (PendingReport).
void FinishPendingReport(const View& view) {
  if (!g_pendingReport || view.delaysRound == g_pendingReport->round || view.delaysTesting) {
    return;
  }
  const PendingReport pending = *std::exchange(g_pendingReport, std::nullopt);
  UpdateWindows();  // the button is back
  const auto profile = std::find_if(view.profiles.begin(), view.profiles.end(),
                                    [&](const View::ProfileView& p) { return p.id == pending.profileId; });
  if (profile != view.profiles.end()) {
    WriteSupportReport(IsWindow(pending.owner) ? pending.owner : nullptr, *profile, view);
  }
}

// A command from the window. `trayWindow` is the hidden
// window that owns the icon; menus and dialogs belong to args.owner if set.
// A command about one configuration, from its row or its page.
void OnProfileCommand(HWND owner, UiCommand command, const sovereign::tray::UiArgs& args,
                      const View::ProfileView& profile, const View& view) {
  const auto change = [&](ProfileChange::What what, bool on, std::string text = {}) {
    RequestProfileChange({.id = profile.id, .what = what, .on = on, .text = std::move(text), .hours = 0});
  };
  switch (command) {
    case UiCommand::ToggleProfile:
      change(ProfileChange::What::Enabled, !profile.enabled);
      break;
    case UiCommand::RenameProfile:  // typed over the page's title
      if (!args.text.empty()) {
        change(ProfileChange::What::Name, false, Narrow(args.text));
      }
      break;
    case UiCommand::RefreshProfile:
      RequestRefresh(profile.id);
      break;
    case UiCommand::SupportReport:
      if (g_pendingReport) {
        break;  // one is on its way
      }
      if (view.display == Display::On && !view.protocols.empty()) {
        g_pendingReport = PendingReport{.profileId = profile.id, .owner = owner, .round = view.delaysRound};
        RequestUrlTest();
        UpdateWindows();  // the button says it's testing
      } else {
        WriteSupportReport(owner, profile, view);
      }
      break;
    case UiCommand::ChooseVia: {  // directly, or through another configuration that isn't a chain itself
      std::vector<std::wstring> names = {L"Напрямую"};
      std::vector<std::string> ids = {std::string()};
      int current = profile.via.empty() ? 0 : -1;
      for (const auto& other : view.profiles) {
        if (other.id == profile.id || !other.via.empty()) {
          continue;
        }
        current = other.id == profile.via ? static_cast<int>(ids.size()) : current;
        names.push_back(Widen(other.name) + (other.enabled ? L"" : L" (выключена)"));
        ids.push_back(other.id);
      }
      if (const auto picked = PickFromMenu(owner, args.anchor, names, current)) {
        change(ProfileChange::What::Via, false, ids[*picked]);
      }
      break;
    }
    case UiCommand::RefreshDirect:  // from its menu, offered only when it was held back
      if (MessageBoxW(owner,
                      L"Сервер этой подписки увидит твой настоящий IP-адрес. Скачать напрямую?\n\n"
                      L"Без этого она скачается сама, как только будет подключение или пересыльщик (Настройки).",
                      L"Sovereign", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
        RequestRefresh(profile.id, /*direct=*/true);
      }
      break;
    case UiCommand::ToggleAutoUpdate:
      change(ProfileChange::What::AutoUpdate, !profile.autoUpdate);
      break;
    case UiCommand::ChooseRefreshPeriod:
      if (const auto hours = ChooseRefreshPeriod(owner, args.anchor, profile); hours == kCustomPeriod) {
        // Any number of hours, typed over the button.
        if (g_mainWindow != nullptr) {
          const int shown = profile.userHours > 0 ? profile.userHours : profile.serverHours;
          g_mainWindow->EditInPlace(UiCommand::ChooseRefreshPeriod, args.index, UiCommand::SetRefreshHours,
                                    std::to_wstring(shown), /*digitsOnly=*/true);
        }
      } else if (hours) {
        RequestProfileChange({.id = profile.id, .what = ProfileChange::What::Hours, .on = false, .text = {},
                              .hours = *hours});
      }
      break;
    case UiCommand::SetRefreshHours: {
      int hours = 0;
      const std::string text = Narrow(args.text);
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), hours);
      if (error != std::errc{} || end != text.data() + text.size() || hours < 1 ||
          hours > sovereign::tray::kMaxRefreshHours) {
        if (g_trayIcon != nullptr) {
          g_trayIcon->Balloon(L"Sovereign", L"Период — число часов от 1 до 720.");
        }
        break;
      }
      RequestProfileChange({.id = profile.id, .what = ProfileChange::What::Hours, .on = false, .text = {},
                            .hours = hours});
      break;
    }
    case UiCommand::CopyProfileLink:
      if (!profile.url.empty() && CopyText(owner, Widen(profile.url)) && g_trayIcon != nullptr) {
        g_trayIcon->Balloon(L"Sovereign: ссылка скопирована",
                            L"В ней твой ключ доступа — отдавай только своим устройствам.", NIIF_INFO);
      }
      break;
    case UiCommand::RemoveProfile: {
      const std::wstring question = L"Удалить конфигурацию «" + Widen(profile.name) + L"»?\n\n" +
                                    (profile.url.empty() ? L"" : L"Подписка больше не будет обновляться. ") +
                                    L"Её config.json останется в папке history на всякий случай.";
      if (MessageBoxW(owner, question.c_str(), L"Sovereign", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
        RequestRemoveProfile(profile.id);
      }
      break;
    }
    case UiCommand::ToggleServer:
      if (args.sub >= 0 && static_cast<std::size_t>(args.sub) < profile.servers.size()) {
        const View::ServerView& server = profile.servers[static_cast<std::size_t>(args.sub)];
        change(ProfileChange::What::Server, !server.enabled, server.tag);
      }
      break;
    case UiCommand::TakeSubscription:
      RequestChoice(profile.id, ConfigChoice::TakeNew);
      break;
    case UiCommand::KeepConfig:
      RequestChoice(profile.id, ConfigChoice::KeepMine);
      break;
    case UiCommand::CarryOverEdits:
      RequestChoice(profile.id, ConfigChoice::CarryOver);
      break;
    case UiCommand::RevertConfig:
      if (MessageBoxW(owner,
                      L"Вернуть конфиг к подписке в том виде, как она пришла?\n\n"
                      L"Твои правки не пропадут: нынешний config.json сохранится в папке history.",
                      L"Sovereign", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
        RequestChoice(profile.id, ConfigChoice::Revert);
      }
      break;
    default:
      break;
  }
}

// A command from the routing's page.
void OnRoutingCommand(HWND owner, UiCommand command, const sovereign::tray::UiArgs& args, const View& view) {
  using What = RoutingChange::What;
  const auto& r = view.routing;
  const auto send = [](What what, bool on = false, int value = 0, int index = 0, std::string text = {}) {
    RequestRoutingChange({.what = what, .on = on, .value = value, .index = index, .text = std::move(text)});
  };
  POINT cursor{};
  GetCursorPos(&cursor);
  switch (command) {
    case UiCommand::ToggleRussiaDirect: send(What::RussiaDirect, !r.russiaDirect); break;
    case UiCommand::ToggleBlockAds: send(What::BlockAds, !r.blockAds); break;
    case UiCommand::ToggleBlockQuic: send(What::BlockQuic, !r.blockQuic); break;
    case UiCommand::ToggleFinalDirect: send(What::FinalDirect, !r.finalDirect); break;
    case UiCommand::ToggleIpv4Only: send(What::Ipv4Only, !r.ipv4Only); break;
    case UiCommand::ToggleWarp: send(What::Warp, !r.warp); break;
    case UiCommand::AddWarpSite:  // typed over WARP's button, no menu: it goes through WARP
      if (g_mainWindow != nullptr) {
        g_mainWindow->EditInPlace(UiCommand::AddWarpSite, 0, UiCommand::AddWarpSiteText, L"", false);
      }
      break;
    case UiCommand::AddWarpSiteText:
      if (!sovereign::tray::ParseRule(Narrow(args.text), sovereign::tray::RouteRule::Action::Warp)) {
        if (g_trayIcon != nullptr) {
          g_trayIcon->Balloon(L"Sovereign", L"Не понял: нужны сайты (example.com), IP-подсети или программы (.exe).");
        }
      } else {
        send(What::AddRule, false, static_cast<int>(sovereign::tray::RouteRule::Action::Warp), 0, Narrow(args.text));
      }
      break;
    case UiCommand::ChooseWarpVia:
      if (const auto picked = PickFromMenu(owner, args.anchor, {L"через прокси", L"напрямую"}, r.warpViaProxy ? 0 : 1)) {
        send(What::WarpVia, *picked == 0);
      }
      break;
    case UiCommand::ChooseRemoteDns:
      if (const auto picked = PickFromMenu(owner, args.anchor, {L"Cloudflare (DoH)", L"Google (DoH)", L"Quad9 (DoH)"},
                                           static_cast<int>(r.remoteDns))) {
        send(What::RemoteDns, false, static_cast<int>(*picked));
      }
      break;
    case UiCommand::ChooseLocalDns:
      if (const auto picked = PickFromMenu(owner, args.anchor,
                                           {L"Cloudflare (DoH, зашифрованный)", L"Google (DoH, зашифрованный)",
                                            L"Системный (провайдера)"},
                                           static_cast<int>(r.localDns))) {
        send(What::LocalDns, false, static_cast<int>(*picked));
      }
      break;
    case UiCommand::AddRule:  // what to match, typed over the button; then where it goes
      if (g_mainWindow != nullptr) {
        g_mainWindow->EditInPlace(UiCommand::AddRule, 0, UiCommand::AddRuleText, L"", false);
      }
      break;
    case UiCommand::AddRuleText:
      if (!sovereign::tray::ParseRule(Narrow(args.text), sovereign::tray::RouteRule::Action::Direct)) {
        if (g_trayIcon != nullptr) {
          g_trayIcon->Balloon(L"Sovereign", L"Не понял правило: нужны сайты (example.com), IP-подсети или программы (.exe).");
        }
      } else if (const auto picked = PickFromMenu(owner, cursor, RuleActions(), -1)) {
        send(What::AddRule, false, static_cast<int>(*picked), 0, Narrow(args.text));
      }
      break;
    case UiCommand::RuleMenu: {
      if (args.index < 0 || static_cast<std::size_t>(args.index) >= r.rules.size()) {
        break;
      }
      std::vector<std::wstring> items = RuleActions();
      items.emplace_back(L"Удалить");
      const auto picked =
          PickFromMenu(owner, cursor, items, static_cast<int>(r.rules[static_cast<std::size_t>(args.index)].action));
      if (picked && *picked < RuleActions().size()) {
        send(What::SetRuleAction, false, static_cast<int>(*picked), args.index);
      } else if (picked) {
        send(What::RemoveRule, false, 0, args.index);
      }
      break;
    }
    case UiCommand::AddService: {
      std::vector<std::wstring> names;
      std::vector<std::string> ids;
      for (const auto& s : sovereign::tray::ServiceCatalog()) {
        if (std::none_of(r.services.begin(), r.services.end(),
                         [&](const sovereign::tray::ServiceRoute& x) { return x.id == s.id; })) {
          names.push_back(Widen(std::string(s.name)) + L" — через WARP");
          ids.emplace_back(s.id);
        }
      }
      if (const auto picked = names.empty() ? std::nullopt : PickFromMenu(owner, args.anchor, names, -1)) {
        send(What::AddService, true, 0, 0, ids[*picked]);
      }
      break;
    }
    case UiCommand::ServiceMenu: {
      if (args.index < 0 || static_cast<std::size_t>(args.index) >= r.services.size()) {
        break;
      }
      // Through WARP, through each country a server really comes out in, or away.
      const sovereign::tray::ServiceRoute& s = r.services[static_cast<std::size_t>(args.index)];
      const std::vector<std::string> countries = KnownExitCountries(view);
      std::vector<std::wstring> items = {L"Через WARP"};
      int current = s.way == sovereign::tray::ServiceRoute::Way::Warp ? 0 : -1;
      for (const std::string& code : countries) {
        current = s.way == sovereign::tray::ServiceRoute::Way::Country && s.country == code ? static_cast<int>(items.size())
                                                                                            : current;
        items.push_back(L"Через " + Widen(sovereign::tray::CountryName(code)));
      }
      items.emplace_back(L"Убрать");
      const auto picked = PickFromMenu(owner, cursor, items, current);
      if (!picked) {
        break;
      }
      if (*picked == items.size() - 1) {
        send(What::RemoveService, false, 0, args.index);
      } else {
        send(What::SetServiceWay, true, 0, args.index, *picked == 0 ? std::string("warp") : countries[*picked - 1]);
      }
      break;
    }
    case UiCommand::ImportRules: {
      std::vector<std::wstring> names;
      names.reserve(view.profiles.size());
      for (const auto& p : view.profiles) {
        names.push_back(Widen(p.name));
      }
      if (names.empty()) {
        break;
      }
      if (const auto picked = PickFromMenu(owner, args.anchor, names, -1)) {
        send(What::ImportFrom, false, 0, 0, view.profiles[*picked].id);
      }
      break;
    }
    default:
      break;
  }
}

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
    case UiCommand::ImportFile:
      ImportFile(owner);
      break;
    case UiCommand::ScanScreen:
      ScanScreen();
      break;
    case UiCommand::RunChecks: {
      using sovereign::tray::CheckId;
      StartChecks({CheckId::Exit, CheckId::RussiaDirect, CheckId::DnsLeak, CheckId::WebRtc, CheckId::Ipv6, CheckId::Lan,
                   CheckId::DnsLatency},
                  {});
      break;
    }
    case UiCommand::RunSpeed:
      StartChecks({sovereign::tray::CheckId::Speed}, {});
      break;
    case UiCommand::CheckRoute:  // the address, typed over the button
      if (g_mainWindow != nullptr) {
        g_mainWindow->EditInPlace(UiCommand::CheckRoute, 0, UiCommand::CheckRouteText, L"", false);
      }
      break;
    case UiCommand::CheckRouteText:
      StartChecks({sovereign::tray::CheckId::Route}, Narrow(args.text));
      break;
    case UiCommand::ToggleRussiaDirect:
    case UiCommand::ToggleBlockAds:
    case UiCommand::ToggleBlockQuic:
    case UiCommand::ToggleFinalDirect:
    case UiCommand::ToggleIpv4Only:
    case UiCommand::ToggleWarp:
    case UiCommand::ChooseWarpVia:
    case UiCommand::AddWarpSite:
    case UiCommand::AddWarpSiteText:
    case UiCommand::ChooseRemoteDns:
    case UiCommand::ChooseLocalDns:
    case UiCommand::AddRule:
    case UiCommand::AddRuleText:
    case UiCommand::RuleMenu:
    case UiCommand::AddService:
    case UiCommand::ServiceMenu:
    case UiCommand::ImportRules:
      OnRoutingCommand(owner, command, args, view);
      break;
    case UiCommand::ToggleProfile:
    case UiCommand::RenameProfile:
    case UiCommand::RefreshProfile:
    case UiCommand::RefreshDirect:
    case UiCommand::SupportReport:
    case UiCommand::ChooseVia:
    case UiCommand::ToggleAutoUpdate:
    case UiCommand::ChooseRefreshPeriod:
    case UiCommand::SetRefreshHours:
    case UiCommand::CopyProfileLink:
    case UiCommand::RemoveProfile:
    case UiCommand::ToggleServer:
    case UiCommand::TakeSubscription:
    case UiCommand::KeepConfig:
    case UiCommand::CarryOverEdits:
    case UiCommand::RevertConfig:
      if (args.index >= 0 && static_cast<std::size_t>(args.index) < view.profiles.size()) {
        OnProfileCommand(owner, command, args, view.profiles[static_cast<std::size_t>(args.index)], view);
      }
      break;
    case UiCommand::SetAppsMode:
      RequestApps({args.index == 1 ? AppsMode::Include : AppsMode::Exclude, view.apps, {}});
      break;
    case UiCommand::RemoveApp:
      if (args.index >= 0 && static_cast<std::size_t>(args.index) < view.apps.size()) {
        AppsChange change{view.appsMode, view.apps, {}};
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
    case UiCommand::EditRelayUrl:  // typed over its row; the key over its own
    case UiCommand::EditRelayKey:
      if (g_mainWindow != nullptr) {
        const bool url = command == UiCommand::EditRelayUrl;
        g_mainWindow->EditInPlace(command, 0, url ? UiCommand::EditRelayUrlText : UiCommand::EditRelayKeyText,
                                  url ? Widen(view.relayUrl) : std::wstring(), false);
      }
      break;
    case UiCommand::EditRelayUrlText:
    case UiCommand::EditRelayKeyText: {
      const std::string text = Narrow(args.text);
      const bool url = command == UiCommand::EditRelayUrlText;
      if (url && text.empty()) {
        RequestRelay({.url = std::string(), .key = std::nullopt});  // cleared: no relay
      } else if (url ? !sovereign::tray::IsRelayUrl(text) : !sovereign::tray::IsRelayKey(text)) {
        if (g_trayIcon != nullptr) {
          g_trayIcon->Balloon(L"Sovereign", url ? L"Адрес пересыльщика — https://…, без ? и пробелов."
                                                : L"Ключ — от 16 до 256 знаков, без пробелов.");
        }
      } else if (url) {
        RequestRelay({.url = sovereign::tray::RelayBase(text), .key = std::nullopt});
      } else {
        RequestRelay({.url = std::nullopt, .key = text});
      }
      break;
    }
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
                        L"Нажми, чтобы обновить — установщик скачается с GitHub.", NIIF_INFO);
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
      FinishPendingReport(view);
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
    g_checks.reset();  // stops and joins a check run, while its state is still here
  }  // joins the worker before its state goes away
  g_updater = nullptr;
  g_mainWindow = nullptr;
  return exitCode;
}
