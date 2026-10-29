#pragma once

// windows.h's min/max macros break std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tray_model.h"
#include "update.h"

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
// Off: a development build, which never checks (update.h IsDevelopmentBuild).
// Waiting: downloaded unasked, it waits for the computer to be quiet
// (update.h QuietForUpdate) - or for a click.
enum class UiUpdate : std::uint8_t { Idle, Checking, UpToDate, Available, Downloading, Waiting, Failed, Off };

// Where a server is: its exit's country and the network it's in, as looked
// up through it - not what its name says.
struct UiLocation {
  std::wstring country;  // "EE", or empty
  std::wstring isp;      // "Brainoza" (exit_ip.h ShortIsp), or empty
  std::wstring ip;       // the exit's address, for copying
};

// A device of the local network, or an address let into it, as the
// settings list them (lan.h LanEntry).
struct UiLanHost {
  std::wstring address;
  std::wstring name;  // what it was let in under; may be empty
  std::wstring mac;
  bool router = false;
  bool allowed = false;
  bool seen = false;  // around now
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
  // The same, apart - the main screen's card shows each in its place.
  std::wstring trafficUsed;   // "3,2 ГБ"; empty: the panel didn't say
  std::wstring trafficTotal;  // "100,0 ГБ"; empty: no limit (or not said)
  float trafficShare = -1;    // used of the total, 0..1; -1: no total
  std::wstring paidTill;      // "01.01.2027"; empty: not said
  int daysLeft = -1;          // whole days started till then, 0 once past; -1: not said
  bool paidByHand = false;    // the user said it (their own server), not the panel: theirs to change
  std::size_t serverCount = 0;
  bool support = false;    // its panel gave a link to its support (Support-Url)
};
// The routing's page (routing.h).
struct UiRule {
  std::wstring text;    // "qwen.ai, alicdn.com"
  std::wstring action;  // "напрямую" / "через VPN" / "блокировать" / "через WARP"
  bool warp = false;    // "через WARP": listed in WARP's own card while it's on
};
struct UiService {
  std::wstring name;  // "Netflix"
  std::wstring way;   // "через WARP"
};
// A thing on the routing board: a service, a site or a program (a rule), one
// of the lists - Russian sites, ads, cookie windows (the local network is the
// settings', since 17.10.2026). Its bucket is where it goes, a
// RouteRule::Action's number: 0 directly, 1 through the proxy, 2 blocked,
// 3 through WARP.
struct UiChip {
  enum class Kind : std::uint8_t { Service, Rule, Russia, Ads, Cookies };
  Kind kind = Kind::Rule;
  int index = 0;        // Service: into the routing's services; Rule: into its rules
  int bucket = 1;
  std::wstring text;    // "Netflix", "bank.ru", "chrome.exe", "Российские сайты"
  std::wstring detail;  // through a country: "США"
  std::wstring icon;    // a program's exe, for its icon; empty: none known
  std::string logo;     // a service's id, for its logo (icons.h ServiceLogo); empty: none
  // In the bucket everything else goes to, changing nothing - a program there
  // (its rule is the last before that very default), a service there (not
  // through a country of its own), the Russian sites when everything else
  // goes their way and no program goes another (but blocked): kept for the
  // other mode, not shown.
  bool idle = false;
  // Service: the rule of its own program in the same bucket (Telegram.exe by
  // Telegram), folded into it - one thing on the board, its menu moves and
  // removes both; -1: none.
  int program = -1;
};
inline constexpr int kBucketDirect = 0;
inline constexpr int kBucketProxy = 1;
inline constexpr int kBucketBlock = 2;
inline constexpr int kBucketWarp = 3;

struct UiRouting {
  int russia = kBucketDirect;  // the Russian sites' way, a bucket; -1: none of their own
  bool blockAds = true;
  int quic = 0;             // RoutingSettings::Quic's number: 0 auto, 1 blocked, 2 let through
  bool quicBlocked = true;  // what it comes to - auto: by the servers
  int hysteriaMbps = 0;     // RoutingSettings::hysteriaMbps
  bool finalDirect = false;
  bool ipv4Only = true;
  std::wstring remoteDns;  // "Cloudflare"
  std::wstring localDns;   // "Cloudflare (DoH)"
  std::string remoteDnsLogo;  // its logo (icons.h ServiceLogo): "cloudflare"; empty: none
  std::string localDnsLogo;   // the one running; the system's resolver has none
  std::vector<UiRule> rules;
  std::wstring lists;      // "списки обновлены 01.10 14:00", "скачиваются: 2 из 4"
  bool listsFailed = false;
  // Cloudflare WARP (warp.h).
  bool warp = false;          // on (registered, or being registered)
  bool warpRegistered = false;
  bool warpViaProxy = true;
  // How its tunnel is (main.cpp WatchWarp): "работает через «NL»", "не отвечает",
  // "сейчас напрямую: через VPN не прошёл"; empty: not checked yet.
  std::wstring warpState;
  std::vector<UiService> services;  // sent their own way, in order
  std::vector<UiChip> board;        // everything sent somewhere, as the board shows it
};

// The checks' page (diagnostics.h): one row each.
struct UiCheck {
  enum class Status : std::uint8_t { NotRun, Running, Ok, Warn, Fail };
  // A failed one's way out (diagnostics.h CheckFix): the page offers it under the results.
  enum class Fix : std::uint8_t { None, Reconnect, KillSwitch, Routing, Dns, LocalNetwork };
  std::wstring title;
  std::wstring summary;
  std::wstring detail;
  Status status = Status::NotRun;
  std::wstring hint;  // what it checks and how - its "?"
  Fix fix = Fix::None;
  // The speed test's throughput, Mbit/s every quarter of a second (diagnostics.h kSpeedSampleMs).
  std::vector<float> speedDown;
  std::vector<float> speedUp;
};

// A question the window asks over itself (MainWindow::Ask) - its own dark
// card in place of the system's light MessageBox. no empty: a note with one
// button (yes).
struct UiQuestion {
  enum class Tone : std::uint8_t { Accent, Warning, Danger };
  std::wstring title;  // "Закрыть Sovereign?"
  std::wstring text;   // what it means, under it
  std::wstring yes;    // "Закрыть"
  std::wstring no = L"Отмена";
  std::wstring other{};  // a second way to go on, under yes and over no; empty: none
  Tone tone = Tone::Accent;
  std::wstring glyph{};  // a Segoe Fluent Icons character, in a circle before the title; empty: a "!"
};

// What the tray's window shows; built by main.cpp from the worker's view.
// Strings are ready to draw.
struct UiContent {
  Display display = Display::ServiceDown;
  bool on = false;               // the toggle's position (the user's intent)
  std::int64_t onSince = 0;      // unix seconds it's been connected since; 0: not connected
  double down = 0;               // bytes per second, now
  double up = 0;
  // The live traffic round the switch: bytes per second through the VPN,
  // directly, through WARP (up and down together), and the ads blocked so far
  // (a spark a new one).
  double flowVpn = 0;
  double flowDirect = 0;
  double flowWarp = 0;
  std::uint64_t blockedCount = 0;
  std::wstring error;            // why the box doesn't run; empty if it does or is off
  // A subscription in trouble (support_report.h SubscriptionTrouble), said on
  // the main screen with its report a click away: "«name»: не отвечает ни один
  // сервер"; empty while all is fine. troubleProfile: its index in profiles.
  std::wstring trouble;
  int troubleProfile = -1;
  // No configuration yet: what the clipboard holds that could be one
  // (DescribeImport: "подписку с packetlab.tech"); empty if nothing.
  std::wstring clipboardOffer;
  bool troubleSupport = false;  // its panel gave a support link
  bool reportPending = false;   // a support report waits for its fresh test of the servers

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
  // What each option is, by its config (protocol_choice.h ServerTitles):
  // shown instead of its name ("" - unknown: the name). Under it its note:
  // the name, when it says more than the title ("4G | Whitelist №2" - not
  // "AnyTLS-REALITY"), and the host for one still not told from another.
  std::vector<std::wstring> protocolTitles;
  std::vector<std::wstring> protocolNotes;
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
  bool delaysDetailed = false;          // latency in full (DelayLabel, DelayDetail)
  bool serversByDelay = true;           // the servers' page lists them fastest first (ServerOrder)
  // The order the servers' page shows them in, indices into protocols (the
  // window's own: kept through a test, while every figure is "…"); empty:
  // ServerOrder's.
  std::vector<std::size_t> serverOrder;
  std::wstring logLevel;                // what the core writes; empty = the config's
  bool killSwitch = false;              // the setting
  bool lanClosed = false;               // the local network closed both ways (settings.h)
  std::vector<UiLanHost> lanHosts;      // the router, what's let in, the rest around (lan.h LanEntries)
  // The subscription relay (relay.h): its host only - the key never reaches the UI.
  std::wstring relayHost;
  bool relaySet = false;
  bool sharedRelay = false;  // Sovereign's own relay chosen (relay.h kSovereignRelayUrl)
  bool killSwitchActive = false;        // the filters are in place
  std::wstring killSwitchError;         // why the service couldn't apply it
  bool autostart = false;               // the tray starts when the user signs in
  bool connectAtStartup = true;         // and connects (if it was on), else starts turned off
  bool routingMore = false;             // the settings' fine settings (QUIC, IPv4, DNS) unfolded
  bool settingsMore = false;            // the configurations' relay fold open (the window's own)
  std::optional<UiQuestion> question;   // asked over the page (the window's own, MainWindow::Ask)
  bool lockedOut = false;               // on, the kill switch holding the internet shut, nothing getting through
  std::wstring lockoutReason;           // why nothing gets through
  std::wstring localNetwork;            // the user's own network bad around the proxy too: said so; empty if fine
  std::wstring version;                 // Sovereign's, "0.1.0"
  std::wstring boxVersion;              // the sing-box inside, "1.14.1"
  // That sing-box against its newest stable release, once the updater asked:
  // the newest (true) or behind boxLatest - which then waits for a Sovereign
  // release that carries it.
  std::optional<bool> boxIsLatest;
  std::wstring boxLatest;
  UiUpdate update = UiUpdate::Idle;
  UpdateMode updateMode = UpdateMode::Check;  // what the updater does unasked
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
  AddMenu,           // anchor: the three ways to add one - the clipboard, a QR code on the screen, a file
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
  ChooseQuic,         // anchor: where to open the menu
  SetHysteriaMbps,      // index: Hysteria 2's speed, Mbit/s (kHysteriaSpeeds); 0: auto (BBR)
  SetRouteMode,        // index: 0 everything through the VPN but what's sent elsewhere, 1 only what's sent through it
  ToggleRoutingMore,   // the fine settings and DNS, unfolded or not
  ToggleIpv4Only,
  ToggleWarp,         // on: registers a WARP device first if there's none
  ChooseWarpVia,      // anchor: over the proxy or directly
  AddWarpSite,        // starts typing over WARP's button
  AddWarpSiteText,    // text: sites (or subnets, programs) to send through WARP
  ChooseRemoteDns,    // anchor: where to open the menu
  ChooseLocalDns,     // anchor
  RemoteDnsText,      // text: a DNS server of the user's own, typed over the row (routing.h ParseCustomDns)
  LocalDnsText,       // text: the same, for direct names
  AddRule,            // starts typing over the button
  AddRuleText,        // text: what was typed
  RuleMenu,           // index into the rules; anchor
  AddService,         // anchor: the menu of services not sent their own way yet
  ServiceMenu,        // index into the services; anchor
  ImportRules,        // anchor: the menu of configurations to take rules from
  BoardAdd,           // index: a bucket (UiChip::bucket); anchor: what can go there - a site typed across it
  BoardSiteText,      // index: the bucket; text: sites (or subnets) typed
  ChipMenu,           // index: into the board; anchor: where it goes instead, or away
  ChipRuleText,       // index: into the board (a rule's chip); text: its sites and programs typed anew
  ChipRuleName,       // index: into the board (a rule's chip); text: its name typed (empty: none)
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
  ToggleDelayDetails,  // the servers' latency in full, or one rounded figure
  SetServerOrder,      // index: 0 fastest first, 1 the subscription's order
  ChooseLogLevel,      // anchor: where to open the menu of levels
  ToggleKillSwitch,
  ToggleLanClosed,
  LanChipMenu,        // index into lanHosts: a device let in; anchor: shut again
  LanAdd,             // anchor: who to let into the closed network - a device around, an address typed
  AddLanHostText,     // text: the address
  PaidMenu,           // index: a configuration; what the user paid its server till - typed, a month more, none
  PaidDateText,       // index: a configuration; text: the date, "15.11" or "15.11.2026"
  EditRelayUrl,       // starts typing the relay's address over its row
  EditRelayUrlText,   // text: the address ("" clears the relay)
  EditRelayKey,       // starts typing the relay's key over its row (shown empty)
  EditRelayKeyText,   // text: the key
  ToggleSharedRelay,  // Sovereign's own relay, on or off
  RefreshDirect,      // index into profiles: fetch it directly - its server sees the user's IP
  ChooseVia,          // index into profiles, anchor: the menu of configurations to connect through
  SetAppsMode,   // index: 0 all except the list, 1 only the list
  RemoveApp,     // index into apps
  AddRunning,    // anchor: where to open the list of running programs
  AddExe,
  SetProtocol,   // index into protocols
  ToggleAutostart,
  ChooseStartup,       // anchor: where to open the menu
  OpenWithoutVpn,      // off: the kill switch's filters go with it
  TestDelays,
  CheckUpdate,
  InstallUpdate,
  ChooseUpdates,       // anchor: where to open the menu - looked for unasked or not, installed or not
  OpenWindow,    // index: the UiPage to show
  OpenFolder,
  Exit,
  Reconnect,     // the connection stopped and started again with the config as it is (the checks' way out)
  ToolsMenu,     // anchor: the main screen's tools - the checks, the log, the report
  ReportProblem, // a GitHub issue opened with the client's report filled in (problem_report.h)
};

struct UiArgs {
  POINT anchor{};
  int index = 0;
  int sub = 0;  // a second index: a server within a configuration
  std::wstring text;  // what was typed in place (MainWindow::EditInPlace)
  HWND owner = nullptr;  // the window a menu or a dialog belongs to
};

// UiArgs::index of ToggleKillSwitch from the checks' banner (not the
// settings' switch): the protection is looked at again after it.
inline constexpr int kFromChecks = 1;

// Hysteria 2's speeds to pick from (UiCommand::SetHysteriaMbps), Mbit/s; 0: auto (BBR).
inline constexpr std::array kHysteriaSpeeds = {0, 50, 100, 200, 300, 500, 1000};

// "48 ±3 мс" (`detailed`) or "50 мс" - rounded to 5 ms: two measurements of
// one server a moment apart differ by more than that (09.10.2026, four
// protocols to one server read 48 to 62 ms) - "нет ответа", "…" or nothing.
inline std::wstring DelayLabel(const UiDelay& delay, bool detailed) {
  switch (delay.state) {
    case UiDelay::State::None: return {};
    case UiDelay::State::Pending: return L"…";
    case UiDelay::State::Ok:
      if (!detailed) {
        return std::format(L"{} мс", delay.ms < 5 ? delay.ms : (delay.ms + 2) / 5 * 5);
      }
      return delay.jitter > 0 ? std::format(L"{} ±{} мс", delay.ms, delay.jitter) : std::format(L"{} мс", delay.ms);
    case UiDelay::State::Failed: return L"нет ответа";
  }
  return {};
}

// What the figures of a measured server come to, in a word: losing requests
// ("теряет 16%": from 10%), jumping (the spread a third of the latency or
// more, and 30 ms), else steady. The ± and the count of requests are the
// working behind it (DelayDetail, detailed).
inline std::wstring DelayVerdict(const UiDelay& delay) {
  if (delay.state != UiDelay::State::Ok) {
    return {};
  }
  if (delay.loss >= 10) {
    return std::format(L"теряет {}%", delay.loss);
  }
  if (delay.jitter >= 30 && delay.jitter * 3 >= delay.ms) {
    return L"скачет";
  }
  return L"стабильный";
}

// Why a server gave no answer, short: the core's error is a Go one -
// `Head "https://www.gstatic.com/generate_204": context deadline exceeded` -
// the sort of text nobody reads. Unknown ones are cut after the quoted URL.
inline std::wstring ShortDelayError(const std::wstring& error) {
  const auto has = [&error](const wchar_t* part) { return error.find(part) != std::wstring::npos; };
  if (has(L"deadline") || has(L"timeout") || has(L"timed out")) {
    return L"не отвечает";
  }
  if (has(L"refused") || has(L"Application error") || has(L"reset") || has(L"EOF")) {
    return L"сервер отклонил соединение";
  }
  if (has(L"tls") || has(L"TLS") || has(L"certificate") || has(L"handshake")) {
    return L"не прошёл TLS";
  }
  if (has(L"no such host") || has(L"lookup")) {
    return L"имя сервера не найдено";
  }
  const auto quote = error.find(L"\": ");
  return quote == std::wstring::npos ? error : error.substr(quote + 3);
}

// What a measurement says: "стабильный" - and, `detailed`, what it is made of:
// "замеров: 9 · соединение 412 мс" - or why there was no answer (the core's
// words as they are, `detailed`).
inline std::wstring DelayDetail(const UiDelay& delay, bool detailed) {
  if (delay.state == UiDelay::State::Failed) {
    return detailed ? delay.error : ShortDelayError(delay.error);
  }
  if (delay.state != UiDelay::State::Ok) {
    return {};
  }
  std::wstring text = DelayVerdict(delay);
  if (!detailed) {
    return text;
  }
  text += std::format(L" · замеров: {}", delay.samples);
  if (delay.connect > 0) {
    text += std::format(L" · соединение {} мс", delay.connect);
  }
  return text;
}

// The servers as their page lists them, indices into c.protocols, auto left
// out (it has a card of its own): with serversByDelay the answering ones
// fastest first, then the ones not measured, then the ones that didn't
// answer - each in the subscription's order among its equals; else the
// subscription's order.
inline std::vector<std::size_t> ServerOrder(const UiContent& c) {
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < c.protocols.size(); ++i) {
    if (static_cast<int>(i) != c.autoOption) {
      order.push_back(i);
    }
  }
  if (!c.serversByDelay) {
    return order;
  }
  const auto rank = [&c](std::size_t i) {
    const UiDelay* delay = i < c.delays.size() ? &c.delays[i] : nullptr;
    if (delay == nullptr || (delay->state != UiDelay::State::Ok && delay->state != UiDelay::State::Failed)) {
      return std::pair{1, 0};
    }
    return delay->state == UiDelay::State::Ok ? std::pair{0, delay->ms} : std::pair{2, 0};
  };
  std::stable_sort(order.begin(), order.end(), [&rank](std::size_t a, std::size_t b) { return rank(a) < rank(b); });
  return order;
}

// "день", "дня" or "дней" after the number `n`.
inline const wchar_t* DaysWord(std::int64_t n) {
  const std::int64_t last = n % 10;
  const std::int64_t lastTwo = n % 100;
  if (last == 1 && lastTwo != 11) {
    return L"день";
  }
  if (last >= 2 && last <= 4 && (lastTwo < 12 || lastTwo > 14)) {
    return L"дня";
  }
  return L"дней";
}

// "осталось 6 дней", "остался 1 день" - till a payment.
inline std::wstring DaysLeftText(std::int64_t n) {
  return std::format(L"{} {} {}", n % 10 == 1 && n % 100 != 11 ? L"остался" : L"осталось", n, DaysWord(n));
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
