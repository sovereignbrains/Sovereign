#pragma once

// windows.h's min/max macros break std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "tray_model.h"

namespace sovereign::tray {

// A server's latency as the windows show it.
struct UiDelay {
  enum class State : std::uint8_t { None, Pending, Ok, Failed };
  State state = State::None;  // None: not tested since the box started
  int ms = 0;            // Ok: the median round trip
  int jitter = 0;        // Ok: half the spread of the round trips
  int loss = 0;          // Ok: percent of the requests lost
  int samples = 0;       // Ok: round trips measured
  int connect = 0;       // a connection with TLS and a first request; 0 if unknown
  std::wstring error{};  // Failed
};

// Where the updater is (updater.h), as the windows show it.
enum class UiUpdate : std::uint8_t { Idle, Checking, UpToDate, Available, Downloading, Failed };

// Where a server is: its exit's country and the network it's in, as looked
// up through it - not what its name says.
struct UiLocation {
  std::wstring country;  // "EE", or empty
  std::wstring isp;      // "Brainoza" (exit_ip.h ShortIsp), or empty
  std::wstring ip;       // the exit's address, for copying
};

// A server of a configuration, as its page lists it.
struct UiServer {
  std::wstring name;   // its tag
  std::wstring label;  // "VLESS · REALITY"
  bool enabled = true;
};

// A configuration (profiles.h): its line in the list and its page.
struct UiProfile {
  std::string id;          // which one a page shows, across updates
  std::wstring name;
  std::wstring detail;     // the list's line: "подписка · 8 серверов · 30.09 14:20"
  bool enabled = true;     // its servers are in the config the box runs
  bool subscription = false;
  bool failed = false;     // its last refresh failed
  std::wstring host;       // the subscription server's name only - the URL's path is a secret
  std::wstring updated;    // "30.09 14:20" / "ещё не загружена"
  bool autoUpdate = true;
  std::wstring period;     // "каждые 12 ч (как советует сервер)"
  std::wstring error;      // why the last refresh failed
  bool edited = false;     // config.json has edits of the user's own (config_sync.h)
  bool waiting = false;    // a newer subscription waits for the user's choice
  std::wstring choiceError;
  std::vector<std::wstring> mergeNotes;  // where carrying the edits over met the subscription's changes
  std::vector<UiServer> servers;
  bool heldBack = false;   // not fetched so its server doesn't see the user's IP: "download directly" offered
  std::wstring via;        // the configuration its servers connect through (its name); empty: directly
  bool viaBroken = false;  // that one is off, gone or chained itself: these servers don't run
  std::wstring usage;      // what its panel said: "Трафик: 3 ГБ из 100,0 ГБ · оплачено до 01.01.2027"; empty: nothing
  bool support = false;    // its panel gave a link to its support (Support-Url)
};
// The routing's page (routing.h).
struct UiRule {
  std::wstring text;    // "qwen.ai, alicdn.com"
  std::wstring action;  // "напрямую" / "через прокси" / "блокировать" / "через WARP"
  bool warp = false;    // "через WARP": listed in WARP's own card while it's on
};
struct UiService {
  std::wstring name;  // "Netflix"
  std::wstring way;   // "через WARP"
};
struct UiRouting {
  bool russiaDirect = true;
  bool blockAds = true;
  bool blockQuic = true;
  bool finalDirect = false;
  bool ipv4Only = true;
  std::wstring remoteDns;  // "Cloudflare"
  std::wstring localDns;   // "Cloudflare (DoH)"
  std::vector<UiRule> rules;
  std::wstring lists;      // "списки обновлены 01.10 14:00", "скачиваются: 2 из 4"
  bool listsFailed = false;
  // Cloudflare WARP (warp.h).
  bool warp = false;          // on (registered, or being registered)
  bool warpRegistered = false;
  bool warpViaProxy = true;
  std::wstring warpAddress;   // the device's address in WARP, once registered
  std::vector<UiService> services;  // sent their own way, in order
};

// The checks' page (diagnostics.h): one row each.
struct UiCheck {
  enum class Status : std::uint8_t { NotRun, Running, Ok, Warn, Fail };
  std::wstring title;
  std::wstring summary;
  std::wstring detail;
  Status status = Status::NotRun;
  std::wstring hint;  // what it checks and how - its "?"
};

// What the tray's window shows; built by main.cpp from the worker's view.
// Strings are ready to draw.
struct UiContent {
  Display display = Display::ServiceDown;
  bool on = false;               // the toggle's position (the user's intent)
  double down = 0;               // bytes per second
  double up = 0;
  std::int64_t connections = 0;
  std::wstring error;            // why the box doesn't run; empty if it does or is off

  // Every configuration; the ones on run together (combine.h).
  std::vector<UiProfile> profiles;
  bool hasConfig = false;        // the ones on make a config to run
  std::wstring combineError;     // why they don't
  std::vector<std::wstring> combineNotes;  // what was left out of it
  UiRouting routing;
  std::vector<UiCheck> checks;  // the "Проверка" page, in diagnose.h's CheckId order
  bool checksRunning = false;

  bool appsInclude = false;      // per-app mode: false = all except the list
  std::vector<std::wstring> apps;
  std::vector<std::wstring> appPaths;   // per app: its exe's full path for the icon; empty if unknown
  std::vector<std::wstring> protocols;  // the selector's options; empty = no choice
  int protocol = -1;                    // index of the one in use
  std::vector<UiDelay> delays;          // one per protocol (may be shorter: untested)
  std::vector<UiLocation> locations;    // one per protocol (may be shorter: not known)
  bool delaysTesting = false;           // a latency test is running
  bool canTestDelays = false;           // the box runs: its servers can be tested
  std::wstring delayError;              // why the last test didn't start
  int autoOption = -1;                  // the protocol that is auto (a URL test), if any
  std::wstring autoServer;              // the server auto runs on; empty until measured
  std::wstring selectError;             // why the box couldn't be switched to the pick
  // The exit through the server in use (while on): address, country.
  std::wstring exitIp;                  // empty until known
  std::wstring exitCountry;             // "NL", or empty
  std::wstring exitCountryName;         // "Нидерланды" - the system's name for it
  bool exitPending = false;             // being looked up
  bool hideExitIp = false;              // the country only
  std::wstring logLevel;                // what the core writes; empty = the config's
  bool killSwitch = false;              // the setting
  bool killSwitchLan = true;
  // The subscription relay (relay.h): its host only - the key never reaches the UI.
  std::wstring relayHost;
  bool relaySet = false;
  bool killSwitchActive = false;        // the filters are in place
  std::wstring killSwitchError;         // why the service couldn't apply it
  bool autostart = false;               // the tray starts when the user signs in
  std::wstring version;                 // "0.1.0 · sing-box 1.14.1"
  UiUpdate update = UiUpdate::Idle;
  std::wstring updateVersion;           // the newest release, once known
  std::wstring updateError;
};

// The window's pages: the overview, and the ones it opens (back with Esc).
// Profile: one configuration's page, opened from the list on Subscription.
// Routing: where traffic goes and DNS - the per-app list too.
enum class UiPage : std::uint8_t { Overview, Servers, Subscription, Logs, Settings, Profile, Routing, Checks };
inline constexpr int kUiPageCount = 5;  // the ones Ctrl+1..5 open: Overview to Settings

enum class UiCommand : std::uint8_t {
  Toggle,
  PasteSubscription,
  ImportFile,        // a config, keys or a QR code's picture from a file
  ScanScreen,        // QR codes on the screen
  // A configuration's, by index into profiles:
  ToggleProfile,     // on or off
  RenameProfile,     // text: the name typed in place
  RefreshProfile,
  ToggleAutoUpdate,
  ChooseRefreshPeriod,  // anchor: where to open the menu of intervals
  SetRefreshHours,   // text: the hours typed in place
  CopyProfileLink,   // the subscription link onto the clipboard
  SupportReport,     // the report for its support saved, a short one copied, support's link opened (support_report.h)
  RemoveProfile,     // after asking
  ToggleServer,      // sub: index into its servers
  // The routing's:
  ToggleRussiaDirect,
  ToggleBlockAds,
  ToggleBlockQuic,
  ToggleFinalDirect,
  ToggleIpv4Only,
  ToggleWarp,         // on: registers a WARP device first if there's none
  ChooseWarpVia,      // anchor: over the proxy or directly
  AddWarpSite,        // starts typing over WARP's button
  AddWarpSiteText,    // text: sites (or subnets, programs) to send through WARP
  ChooseRemoteDns,    // anchor: where to open the menu
  ChooseLocalDns,     // anchor
  AddRule,            // starts typing over the button
  AddRuleText,        // text: what was typed
  RuleMenu,           // index into the rules; anchor
  AddService,         // anchor: the menu of services not sent their own way yet
  ServiceMenu,        // index into the services; anchor
  ImportRules,        // anchor: the menu of configurations to take rules from
  // The checks':
  RunChecks,          // everything but speed and an address
  RunSpeed,
  CheckRoute,         // starts typing an address over the button
  CheckRouteText,     // text: the address
  TakeSubscription,  // the waiting subscription replaces the edited config
  KeepConfig,        // the edited config stays
  CarryOverEdits,    // the edits, merged into the waiting subscription
  RevertConfig,      // the config back to the subscription as it arrived
  ToggleExitIp,        // show or hide the exit's address
  ChooseLogLevel,      // anchor: where to open the menu of levels
  ToggleKillSwitch,
  ToggleKillSwitchLan,
  EditRelayUrl,       // starts typing the relay's address over its row
  EditRelayUrlText,   // text: the address ("" clears the relay)
  EditRelayKey,       // starts typing the relay's key over its row (shown empty)
  EditRelayKeyText,   // text: the key
  RefreshDirect,      // index into profiles: fetch it directly - its server sees the user's IP
  ChooseVia,          // index into profiles, anchor: the menu of configurations to connect through
  SetAppsMode,   // index: 0 all except the list, 1 only the list
  RemoveApp,     // index into apps
  AddRunning,    // anchor: where to open the list of running programs
  AddExe,
  SetProtocol,   // index into protocols
  ToggleAutostart,
  TestDelays,
  CheckUpdate,
  InstallUpdate,
  OpenWindow,    // index: the UiPage to show
  OpenFolder,
  Exit,
};

struct UiArgs {
  POINT anchor{};
  int index = 0;
  int sub = 0;  // a second index: a server within a configuration
  std::wstring text;  // what was typed in place (MainWindow::EditInPlace)
  HWND owner = nullptr;  // the window a menu or a dialog belongs to
};

// "48 ±3 мс", "нет ответа", "…" or nothing.
inline std::wstring DelayLabel(const UiDelay& delay) {
  switch (delay.state) {
    case UiDelay::State::None: return {};
    case UiDelay::State::Pending: return L"…";
    case UiDelay::State::Ok:
      return delay.jitter > 0 ? std::format(L"{} ±{} мс", delay.ms, delay.jitter) : std::format(L"{} мс", delay.ms);
    case UiDelay::State::Failed: return L"нет ответа";
  }
  return {};
}

// What a measurement is made of: "замеров: 9 · соединение 412 мс · потери 10%",
// or why there was no answer.
inline std::wstring DelayDetail(const UiDelay& delay) {
  if (delay.state == UiDelay::State::Failed) {
    return delay.error;
  }
  if (delay.state != UiDelay::State::Ok) {
    return {};
  }
  std::wstring text = std::format(L"замеров: {}", delay.samples);
  if (delay.connect > 0) {
    text += std::format(L" · соединение {} мс", delay.connect);
  }
  if (delay.loss > 0) {
    text += std::format(L" · потери {}%", delay.loss);
  }
  return text;
}

inline std::wstring FormatRate(double bytesPerSecond) {
  if (bytesPerSecond < 1024) {
    return std::format(L"{:.0f} Б/с", bytesPerSecond);
  }
  if (bytesPerSecond < 1024 * 1024) {
    return std::format(L"{:.1f} КБ/с", bytesPerSecond / 1024);
  }
  return std::format(L"{:.1f} МБ/с", bytesPerSecond / (1024 * 1024));
}

}  // namespace sovereign::tray
