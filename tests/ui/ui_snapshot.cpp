// ui-snapshot: the main window's pages drawn into PNGs with sample content,
// no desktop needed - so a change to the window can be looked at from CI's
// artifacts (the build job uploads them) or anywhere else.
//
//   ui-snapshot.exe <out dir>     every page, at 100% and the overview at 150%
//   ui-snapshot.exe --window      the real window with the same content, to click through

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "icons.h"
#include "main_window.h"
#include "routing.h"
#include "ui_content.h"

namespace {

using sovereign::tray::UiContent;
using sovereign::tray::UiPage;
using sovereign::tray::UiCalendar;
using sovereign::tray::UiQuestion;

// The routing board with a bit of everything, a long site among it.
std::vector<sovereign::tray::UiChip> SampleBoard() {
  using sovereign::tray::UiChip;
  return {
      {.kind = UiChip::Kind::Russia, .index = 0, .bucket = 0, .text = L"Российские сайты", .detail = {}, .icon = {}, .logo = "russia"},
      {.kind = UiChip::Kind::Service, .index = 0, .bucket = 3, .text = L"Netflix", .detail = {}, .icon = {}, .logo = "netflix"},
      {.kind = UiChip::Kind::Service, .index = 1, .bucket = 3, .text = L"ChatGPT", .detail = {}, .icon = {}, .logo = "chatgpt"},
      {.kind = UiChip::Kind::Service, .index = 2, .bucket = 1, .text = L"YouTube", .detail = L"США", .icon = {}, .logo = "youtube"},
      {.kind = UiChip::Kind::Service, .index = 3, .bucket = 0, .text = L"Steam", .detail = {}, .icon = {}, .logo = "steam"},
      // Its app's old rule folded into it (Telegram.exe).
      {.kind = UiChip::Kind::Service, .index = 4, .bucket = 3, .text = L"Telegram", .detail = {}, .icon = {},
       .logo = "telegram", .idle = false, .program = 5},
      {.kind = UiChip::Kind::Rule, .index = 0, .bucket = 0, .text = L"qwen.ai, qwenlm.ai, alicdn.com, aliyun.com",
       .detail = {}, .icon = {}, .logo = {}},
      {.kind = UiChip::Kind::Rule, .index = 1, .bucket = 1, .text = L"10.9.0.0/16, steam.exe", .detail = {},
       .icon = {}, .logo = {}},
      {.kind = UiChip::Kind::Rule, .index = 2, .bucket = 2, .text = L"~tracker", .detail = {}, .icon = {}, .logo = {}},
      {.kind = UiChip::Kind::Rule, .index = 3, .bucket = 3, .text = L"chatgpt.com", .detail = {}, .icon = {}, .logo = {}},
      {.kind = UiChip::Kind::Rule, .index = 4, .bucket = 0,
       .text = L"очень-длинный-поддомен.какой-то-банк-с-длинным-именем.example.ru", .detail = {}, .icon = {}, .logo = {}},
  };
}

UiContent SampleContent() {
  UiContent c;
  c.display = sovereign::tray::Display::On;
  c.on = true;
  // The live traffic round the switch: a film playing through the VPN, a
  // little directly and through WARP, two ads blocked.
  c.down = 420000;
  c.flowVpn = 450000;
  c.flowDirect = 30000;
  c.flowWarp = 60000;
  c.blockedCount = 2;
  c.hasConfig = true;
  c.lanClosed = true;
  c.lanHosts = {
      {.address = L"192.168.31.1", .name = {}, .mac = L"a4:39:b3:00:11:22", .router = true, .allowed = false, .seen = true},
      {.address = L"192.168.31.200", .name = L"Принтер", .mac = {}, .router = false, .allowed = true, .seen = false},
      {.address = L"192.168.31.45", .name = {}, .mac = L"3c:22:fb:aa:bb:cc", .router = false, .allowed = false, .seen = true}};
  using sovereign::tray::UiServer;
  sovereign::tray::UiProfile packetlab;
  packetlab.id = "p1";
  packetlab.name = L"packetlab";
  packetlab.detail = L"подписка · серверов: 3 (выкл. 1) · 27.09 19:20";
  packetlab.subscription = true;
  packetlab.host = L"packetlab.tech";
  packetlab.usage = L"Трафик: 3,2 ГБ из 100,0 ГБ · оплачено до 01.01.2027 (ещё 87 дн.)";
  packetlab.trafficUsed = L"3,2 ГБ";
  packetlab.trafficTotal = L"100,0 ГБ";
  packetlab.trafficShare = 0.032f;
  packetlab.paidTill = L"01.01.2027";
  packetlab.daysLeft = 87;
  packetlab.serverCount = 3;
  packetlab.support = true;
  packetlab.updated = L"27.09 19:20";
  packetlab.period = L"каждые 12 ч (как советует сервер)";
  packetlab.servers = {UiServer{L"AnyTLS · Нидерланды", L"AnyTLS · REALITY", true},
                       UiServer{L"AnyTLS · Финляндия", L"AnyTLS", true},
                       UiServer{L"REALITY · Германия", L"VLESS · REALITY", false}};
  sovereign::tray::UiProfile keys;
  keys.id = "p2";
  keys.name = L"NL-1 и ещё 2";
  keys.detail = L"свой конфиг · серверов: 3";
  keys.servers = {UiServer{L"NL-1", L"Trojan · WS", true}, UiServer{L"\U0001F1E9\U0001F1EA DE-2", L"Hysteria2", true},
                  UiServer{L"WARP", L"WireGuard", true}};
  sovereign::tray::UiProfile off;
  off.id = "p3";
  off.name = L"sub.example.com";
  off.enabled = false;
  off.subscription = true;
  off.failed = true;
  off.detail = L"выключена · подписка · серверов: 0";
  c.profiles = {packetlab, keys, off};  c.appsInclude = false;
  c.apps = {L"steam.exe", L"Telegram.exe", L"qbittorrent.exe", L"notepad.exe"};
  // One exe that is there on any Windows: its real icon; the rest aren't known.
  c.appPaths = {L"", L"", L"", L"C:\\Windows\\System32\\notepad.exe"};
  c.protocols = {L"auto", L"AnyTLS · Нидерланды", L"AnyTLS · Финляндия", L"REALITY · Германия", L"REALITY · Польша"};
  // A flag emoji, as subscriptions name servers: drawn from the flags sprite.
  c.protocols.emplace_back(L"\U0001F1EA\U0001F1FA 4G | Whitelist №1");
  // Where they are, looked up: the EU-flagged one is in Estonia.
  c.locations = {{}, {.country = L"DE", .isp = L"Hetzner Online", .ip = L"5.9.1.2"}, {}, {}, {}, {.country = L"EE", .isp = L"Brainoza", .ip = L"5.181.201.59"}};
  c.protocol = 0;
  c.autoOption = 0;
  c.autoServer = L"AnyTLS · Нидерланды";
  using State = sovereign::tray::UiDelay::State;
  const sovereign::tray::UiDelay nl{.state = State::Ok, .ms = 48, .jitter = 3, .samples = 7, .connect = 212};
  c.delays = {nl, nl, {.state = State::Ok, .ms = 310, .jitter = 41, .loss = 13, .samples = 13, .connect = 940},
              {.state = State::Failed, .error = L"i/o timeout"}, {.state = State::Pending}, nl};
  c.canTestDelays = true;
  c.exitIp = L"185.12.34.56";
  c.exitCountry = L"NL";
  c.exitCountryName = L"Нидерланды";
  c.update = sovereign::tray::UiUpdate::Available;
  c.updateVersion = L"0.3.0";
  c.autostart = true;
  c.version = L"0.12.40";  // the widest a version gets soon
  c.boxVersion = L"1.14.11";
  c.boxIsLatest = false;
  c.boxLatest = L"1.15.10";
  return c;
}

std::vector<std::wstring> SampleLogs() {
  return {
      L"19:20:01  INFO   sing-box started (1.14.1)",
      L"19:20:01  INFO   inbound/tun[tun-in]: started at sovereign-tun",
      L"19:20:02  INFO   outbound/anytls[AnyTLS · Нидерланды]: connected",
      L"19:20:05  WARN   dns: exchange failed for telemetry.example: context deadline exceeded",
      L"19:21:14  INFO   router: match process_name=steam.exe => direct",
      L"19:22:40  ERROR  outbound/anytls[AnyTLS · Финляндия]: dial tcp 203.0.113.7:443: i/o timeout",
      L"19:22:41  INFO   urltest[auto]: best AnyTLS · Нидерланды (48 ms)",
  };
}

// The checks: run, one warning, one failing, one running, speed and an address.
// As main.cpp's ContentFrom lays them: the exit, the leaks as one line, the
// speed, why a site won't open (the routing page's).
std::vector<sovereign::tray::UiCheck> SampleChecks() {
  using Check = sovereign::tray::UiCheck;
  return {
      {L"Выход через VPN", L"5.83.147.210 · DE", L"Этот адрес видят зарубежные сайты.", Check::Status::Ok, L"Что проверяет и как.", Check::Fix::None, {}, {}},
      {L"Утечки", L"есть: IPv6", L"DNS: CloudFlare Inc (DE) — нет\nWebRTC: 5.83.147.210 — нет\nIPv6: 2a01:db8::1 · RU — уходит мимо VPN",
       Check::Status::Fail, L"Что проверяет и как.", Check::Fix::KillSwitch, {}, {}},
      {L"Скорость интернета", L"↓ 151 · ↑ 98 Мбит/с · 84 мс", L"Через VPN, до ближайшего узла Cloudflare.",
       Check::Status::Ok, L"Что проверяет и как.", Check::Fix::None,
       {39, 169, 350, 521, 340, 312, 404, 310, 462, 338, 217, 111, 503, 311, 276, 409, 291, 132, 55, 84, 392, 229},
       {25, 221, 728, 506, 12, 177, 636, 523, 177, 139, 390, 718, 257, 118, 206, 603, 366, 18, 158, 684, 454}},
      {L"Не открывается сайт?", L"vk.com: напрямую", L"Ни одно из твоих правил не подошло — решили списки.",
       Check::Status::Ok, L"Что проверяет и как.", Check::Fix::None, {}, {}}};
}

int Snapshots(const std::wstring& dir) {
  const UiContent content = SampleContent();
  const std::vector<std::wstring> logs = SampleLogs();
  constexpr const wchar_t* kNames[sovereign::tray::kUiPageCount] = {L"overview", L"servers", L"subscription",
                                                                     L"logs", L"settings"};
  CreateDirectoryW(dir.c_str(), nullptr);
  for (int p = 0; p < sovereign::tray::kUiPageCount; ++p) {
    const std::wstring path = dir + L"\\" + kNames[p] + L".png";
    sovereign::tray::RenderMainWindowSnapshot(content, static_cast<UiPage>(p), logs, 400, 620, 96, path);
  }
  sovereign::tray::RenderMainWindowSnapshot(content, UiPage::Overview, logs, 600, 930, 144, dir + L"\\overview-150.png");
  // Maximized on a laptop: the navigation kept to the page's width, the switch grown with the window.
  sovereign::tray::RenderMainWindowSnapshot(content, UiPage::Overview, logs, 1280, 800, 96, dir + L"\\overview-wide.png");
  // Each variant below drawn from a copy of its own, in a function of its own:
  // one UiContent on the stack at a time, not fourteen (C6262 in analyze).
  const auto variant = [&content](const auto& draw) {
    UiContent copy = content;
    draw(copy);
  };
  // The first screen: nothing yet - what the clipboard holds offered, or the ways to bring one.
  variant([&](UiContent& first) {
    first.profiles.clear();
    first.trouble.clear();
    first.on = false;
    first.display = sovereign::tray::Display::Off;
    first.clipboardOffer = L"подписку с packetlab.tech";
    sovereign::tray::RenderMainWindowSnapshot(first, UiPage::Overview, logs, 400, 700, 96, dir + L"\\first-offer.png");
    first.clipboardOffer.clear();
    sovereign::tray::RenderMainWindowSnapshot(first, UiPage::Overview, logs, 400, 700, 96, dir + L"\\first-empty.png");
  });
  // A subscription in trouble: the line and its support report on the main screen.
  variant([&](UiContent& troubled) {
    troubled.trouble = L"«packetlab.tech»: не отвечает ни один сервер";
    troubled.troubleProfile = 0;
    troubled.troubleSupport = true;
    sovereign::tray::RenderMainWindowSnapshot(troubled, UiPage::Overview, logs, 400, 760, 96,
                                              dir + L"\\overview-trouble.png");
  });
  // The routing: own, with rules; and a configuration's.
  variant([&](UiContent& routed) {
    routed.routing.remoteDns = L"Cloudflare (DoH)";
    routed.routing.localDns = L"Cloudflare (DoH)";
    routed.routing.rules = {{L"qwen.ai, qwenlm.ai, alicdn.com, aliyun.com", L"напрямую"},
                            {L"10.9.0.0/16, steam.exe", L"через VPN"},
                            {L"~tracker", L"блокировать"},
                            {L"chatgpt.com", L"через WARP", true}};
    routed.routing.warp = true;
    routed.routing.warpRegistered = true;
      routed.routing.warpState = L"через «4G | Whitelist №1»";
    routed.routing.services = {{.name = L"Netflix", .way = L"через WARP"}, {.name = L"ChatGPT", .way = L"через WARP"}};
    routed.routing.board = SampleBoard();
    routed.routing.lists = L"Списки правил на месте, обновлены 01.10 14:00; обновляются раз в сутки.";
    sovereign::tray::RenderMainWindowSnapshot(routed, UiPage::Routing, logs, 400, 1400, 96, dir + L"\\routing.png");
    routed.routing.lists = L"Списки правил не скачались: сервер ответил 503. Пока работают зоны .ru/.рф/.su.";
    routed.routing.listsFailed = true;
    sovereign::tray::RenderMainWindowSnapshot(routed, UiPage::Routing, logs, 400, 1100, 96,
                                              dir + L"\\routing-lists-failed.png");
    // Nothing blocked by hand: the block is a quiet line, not a card.
    routed.routing.lists.clear();
    routed.routing.board = SampleBoard();
    std::erase_if(routed.routing.board, [](const sovereign::tray::UiChip& chip) { return chip.bucket == 2; });
    sovereign::tray::RenderMainWindowSnapshot(routed, UiPage::Routing, logs, 400, 900, 96,
                                              dir + L"\\routing-block-empty.png");
    // His narrow window (350, 31.10.2026): WARP's note - a long server's name - on a line under the
    // name, not cut to "…"; the route's "?" before its button, not on the title.
    routed.routing.warpState = L"через «AnyTLS-Reality | Frankfurt №1»";
    routed.checks = SampleChecks();
    routed.checks[3].status = sovereign::tray::UiCheck::Status::NotRun;
    sovereign::tray::RenderMainWindowSnapshot(routed, UiPage::Routing, logs, 350, 900, 96,
                                              dir + L"\\routing-narrow.png");
  });
  // The checks: run, one warning, one failing, one running, speed and an address.
  variant([&](UiContent& checked) {
    checked.checks = SampleChecks();
    checked.checksRunning = true;
    sovereign::tray::RenderMainWindowSnapshot(checked, UiPage::Checks, logs, 400, 1300, 96, dir + L"\\checks.png");
  });
  // A configuration's page: the first one's.
  sovereign::tray::RenderMainWindowSnapshot(content, UiPage::Profile, logs, 400, 620, 96, dir + L"\\profile.png");
  // The settings whole, one page: every group, the printer let in, an update waiting.
  sovereign::tray::RenderMainWindowSnapshot(content, UiPage::Settings, logs, 400, 860, 96,
                                            dir + L"\\settings-full.png");
  // ...and the fine settings unfolded under it: QUIC, IPv4, DNS.
  variant([&](UiContent& fine) {
    fine.routingMore = true;
    fine.routing.remoteDns = L"авто · Cloudflare";
    fine.routing.localDns = L"авто · Cloudflare";
    sovereign::tray::RenderMainWindowSnapshot(fine, UiPage::Settings, logs, 400, 1000, 96,
                                              dir + L"\\settings-fine.png");
  });
  // Updates installed unasked, one downloaded and waiting for a quiet
  // moment: its key in the bar, "update now" under the foot, the row's "?"
  // open; and looked for only when asked, nothing found yet.
  variant([&](UiContent& updating) {
    updating.updateMode = sovereign::tray::UpdateMode::Install;
    updating.update = sovereign::tray::UiUpdate::Waiting;
    sovereign::tray::RenderMainWindowSnapshot(updating, UiPage::Settings, logs, 400, 860, 96,
                                              dir + L"\\settings-updates-install.png", L"«Только проверять»");
    updating.updateMode = sovereign::tray::UpdateMode::Off;
    updating.update = sovereign::tray::UiUpdate::Idle;
    sovereign::tray::RenderMainWindowSnapshot(updating, UiPage::Settings, logs, 400, 860, 96,
                                              dir + L"\\settings-updates-off.png");
    // A new version on the main screen: its pill with the number, and at
    // the narrowest window the arrow alone.
    updating.update = sovereign::tray::UiUpdate::Available;
    updating.updateVersion = L"0.5.52";
    sovereign::tray::RenderMainWindowSnapshot(updating, UiPage::Overview, logs, 420, 620, 96,
                                              dir + L"\\overview-update.png", L"Доступна версия");
    sovereign::tray::RenderMainWindowSnapshot(updating, UiPage::Overview, logs, 360, 620, 96,
                                              dir + L"\\overview-update-360.png");
  });

  // The states that look different: off with no subscription, and an error.
  UiContent empty;
  empty.display = sovereign::tray::Display::Off;
  empty.version = content.version;
  empty.boxVersion = content.boxVersion;
  sovereign::tray::RenderMainWindowSnapshot(empty, UiPage::Overview, {}, 400, 620, 96, dir + L"\\overview-empty.png");
  variant([&](UiContent& failed) {
    failed.display = sovereign::tray::Display::Error;
    failed.error = L"уже работает другой клиент sing-box с TUN (адаптер sing-tun) — выключи его, Sovereign подключится сам";
    failed.profiles[0].failed = true;
    failed.profiles[0].error = L"сервер ответил 403";
    sovereign::tray::RenderMainWindowSnapshot(failed, UiPage::Overview, logs, 400, 620, 96,
                                              dir + L"\\overview-error.png");
    sovereign::tray::RenderMainWindowSnapshot(failed, UiPage::Profile, logs, 400, 620, 96,
                                              dir + L"\\profile-error.png");
  });
  // A drop with the kill switch on: the internet held closed, and the switch
  // saying so; in settings, both switches.
  variant([&](UiContent& held) {
    held.display = sovereign::tray::Display::Error;
    held.error = L"ядро остановилось";
    held.killSwitch = true;
    held.killSwitchActive = true;
    sovereign::tray::RenderMainWindowSnapshot(held, UiPage::Overview, logs, 400, 620, 96,
                                              dir + L"\\overview-killswitch.png");
    held.killSwitchError = L"WFP: доступ запрещён";
    sovereign::tray::RenderMainWindowSnapshot(held, UiPage::Settings, logs, 400, 620, 96,
                                              dir + L"\\settings-killswitch.png");
  });

  // A newer subscription over the user's edits: the choice; then, narrow,
  // the buttons stacked; and after a carry-over, where both sides met.
  variant([&](UiContent& waiting) {
    waiting.profiles[0].edited = true;
    waiting.profiles[0].waiting = true;
    sovereign::tray::RenderMainWindowSnapshot(waiting, UiPage::Profile, logs, 400, 620, 96,
                                              dir + L"\\profile-waiting.png");
    sovereign::tray::RenderMainWindowSnapshot(waiting, UiPage::Overview, logs, 400, 620, 96,
                                              dir + L"\\overview-waiting.png");
    waiting.profiles[0].choiceError = L"после переноса правок конфиг не годится: в конфиге нет ни одного outbound";
    sovereign::tray::RenderMainWindowSnapshot(waiting, UiPage::Profile, logs, 360, 640, 96,
                                              dir + L"\\profile-waiting-narrow.png");
  });
  // Locked out by the kill switch: why, and the way out.
  variant([&](UiContent& locked) {
    locked.lockedOut = true;
    locked.lockoutReason = L"ни один сервер не отвечает";
    sovereign::tray::RenderMainWindowSnapshot(locked, UiPage::Overview, logs, 400, 620, 96,
                                              dir + L"\\overview-lockout.png");
  });
  // The user's own network slow around the proxy too.
  variant([&](UiContent& slowNet) {
    slowNet.localNetwork =
        L"Тормозит твоя сеть, а не VPN: напрямую до интернета 190 мс (обычно 15). Проверь Wi-Fi или провайдера.";
    sovereign::tray::RenderMainWindowSnapshot(slowNet, UiPage::Overview, logs, 400, 620, 96,
                                              dir + L"\\overview-slow-network.png");
  });
  // The servers' latency in full: the spread, the requests, the connection.
  variant([&](UiContent& detailed) {
    detailed.delaysDetailed = true;
    sovereign::tray::RenderMainWindowSnapshot(detailed, UiPage::Servers, logs, 400, 620, 96,
                                              dir + L"\\servers-detailed.png");
  });
  // ...and as the subscription lists them, not fastest first.
  variant([&](UiContent& listed) {
    listed.serversByDelay = false;
    sovereign::tray::RenderMainWindowSnapshot(listed, UiPage::Servers, logs, 400, 620, 96,
                                              dir + L"\\servers-listed.png");
  });
  // The window's own questions (MainWindow::Ask): the cross's over the
  // overview; a configuration's removal, narrow, its long name wrapped; a note.
  variant([&](UiContent& asked) {
    asked.question = UiQuestion{.title = L"Закрыть Sovereign?",
                                .text = L"Подключение останется: им управляет служба. Значок в трее исчезнет, окно "
                                        L"вернётся, когда запустишь клиент снова. Спрятать окно в трей можно "
                                        L"чёрточкой рядом с крестиком.",
                                .yes = L"Закрыть",
                                .tone = UiQuestion::Tone::Danger,
                                .glyph = L"\xE7E8"};
    sovereign::tray::RenderMainWindowSnapshot(asked, UiPage::Overview, logs, 400, 620, 96,
                                              dir + L"\\question-exit.png");
    asked.question = UiQuestion{.title = L"Удалить «packetlab.tech · 4G | Whitelist №2 · резерв»?",
                                .text = L"Подписка больше не будет обновляться. Её config.json останется в папке "
                                        L"history на всякий случай.",
                                .yes = L"Удалить",
                                .tone = UiQuestion::Tone::Danger,
                                .glyph = L"\xE74D"};
    sovereign::tray::RenderMainWindowSnapshot(asked, UiPage::Profile, logs, 360, 620, 96,
                                              dir + L"\\question-remove-narrow.png");
    asked.question = UiQuestion{.title = L"Не удалось записать файл",
                                .text = L"Может быть, папка только для чтения или файл открыт в другой программе.",
                                .yes = L"Понятно",
                                .no = L"",
                                .tone = UiQuestion::Tone::Warning};
    sovereign::tray::RenderMainWindowSnapshot(asked, UiPage::Logs, logs, 400, 620, 96, dir + L"\\question-note.png");
    // Three ways on: a subscription added with no connection and no relay.
    asked.question = UiQuestion{.title = L"Как скачать подписку?",
                                .text = L"Подключения нет. Напрямую сервер подписки увидит твой настоящий IP-адрес. Через "
                                        L"сервер Sovereign (Cloudflare) он увидит адрес Cloudflare, но ссылку на подписку "
                                        L"увидит тот, кто держит этот сервер, — автор Sovereign. Выбор запомнится: "
                                        L"Настройки → «Скачивать без подключения».",
                                .yes = L"Через сервер Sovereign",
                                .no = L"Не добавлять",
                                .other = L"Напрямую",
                                .tone = UiQuestion::Tone::Warning,
                                .glyph = L"\xE896"};
    sovereign::tray::RenderMainWindowSnapshot(asked, UiPage::Overview, logs, 400, 620, 96,
                                              dir + L"\\question-relay.png");
    // A date on a calendar, in the window's least height and width.
    asked.question = UiQuestion{.title = L"Оплачено до",
                                .text = {},
                                .yes = L"Готово",
                                .other = L"Не следить",
                                .calendar = UiCalendar{.year = 2026, .month = 2, .day = 28,
                                                       .todayYear = 2026, .todayMonth = 2, .todayDay = 9},
                                .glyph = L"\xE787"};
    sovereign::tray::RenderMainWindowSnapshot(asked, UiPage::Profile, logs, 360, 440, 96,
                                              dir + L"\\question-date.png");
    asked.question.reset();
    asked.settingsMore = true;
    asked.sharedRelay = true;
    sovereign::tray::RenderMainWindowSnapshot(asked, UiPage::Subscription, logs, 400, 620, 96,
                                              dir + L"\\relay-open.png");
  });
  // The exit's address hidden: the flag and the country.
  variant([&](UiContent& hidden) {
    hidden.hideExitIp = true;
    sovereign::tray::RenderMainWindowSnapshot(hidden, UiPage::Overview, logs, 400, 620, 96,
                                              dir + L"\\overview-ip-hidden.png");
  });
  // Tips pointed at: a title bar key's (under it, the window ends above),
  // the "+" on the card, the switch, the server, a "?"'s paragraphs, at 150 % too.
  // The new version's key, leftmost in the bar.
  const std::array<std::pair<const wchar_t*, const wchar_t*>, 5> tips = {{
      {L"Доступна версия", L"tip-update"},
      {L"Сообщить о проблеме", L"tip-report"},
      {L"Вставить из буфера:", L"tip-add"},
      {L"Отключить VPN", L"tip-switch"},
      {L"Выбрать сервер", L"tip-server"},
  }};
  for (const auto& [tip, name] : tips) {
    sovereign::tray::RenderMainWindowSnapshot(content, UiPage::Overview, logs, 400, 620, 96,
                                              dir + L"\\" + name + L".png", tip);
  }
  sovereign::tray::RenderMainWindowSnapshot(content, UiPage::Overview, logs, 600, 930, 144, dir + L"\\tip-report-150.png",
                                            L"Сообщить о проблеме");
  variant([&](UiContent& routed) {
    routed.routing.warp = true;
    routed.routing.warpRegistered = true;
    routed.routing.warpState = L"через «4G | Whitelist №1»";
    routed.routing.board = SampleBoard();
    sovereign::tray::RenderMainWindowSnapshot(routed, UiPage::Routing, logs, 400, 900, 96, dir + L"\\tip-hint.png",
                                              L"WARP — бесплатный");
    sovereign::tray::RenderMainWindowSnapshot(routed, UiPage::Routing, logs, 400, 900, 96, dir + L"\\tip-chip.png",
                                              L"Перенести");
  });

  variant([&](UiContent& merged) {
    merged.profiles[0].edited = true;
    merged.profiles[0].mergeNotes = {L"route.rules", L"outbounds[nl].server_port"};
    sovereign::tray::RenderMainWindowSnapshot(merged, UiPage::Profile, logs, 400, 620, 96,
                                              dir + L"\\profile-merged.png");
  });

  // Every service's logo draws (assets/services: what Direct2D's SVG can't
  // draw comes out empty) - at the menus' 16 px and at 150 %.
  std::vector<std::string_view> logos = {"lan", "russia", "ads", "cookies", "hysteria", "cloudflare", "quad9"};  // the board's, the settings'
  for (const auto& service : sovereign::tray::ServiceCatalog()) {
    logos.push_back(service.id);
  }
  for (const std::string_view id : logos) {
    for (const int size : {16, 24}) {
      const std::vector<std::uint32_t> logo = sovereign::tray::ServiceLogo(id, size);
      if (std::none_of(logo.begin(), logo.end(), [](std::uint32_t pixel) { return (pixel >> 24) > 128; })) {
        std::fprintf(stderr, "ui-snapshot: no logo for %.*s at %d px\n", static_cast<int>(id.size()), id.data(), size);
        return 1;
      }
    }
  }
  return 0;
}

int Window(HINSTANCE instance) {
  // The cross asks as the client's does (main.cpp); its yes closes this.
  sovereign::tray::MainWindow* self = nullptr;
  sovereign::tray::MainWindow window(instance, [&self](sovereign::tray::UiCommand c, const sovereign::tray::UiArgs&) {
    if (c == sovereign::tray::UiCommand::Exit && self != nullptr) {
      self->Ask({.title = L"Закрыть Sovereign?",
                 .text = L"Подключение останется: им управляет служба.",
                 .yes = L"Закрыть",
                 .tone = UiQuestion::Tone::Danger,
                 .glyph = L"\xE7E8"},
                [&self] { self->Hide(); });
    }
  });
  self = &window;
  window.Update(SampleContent());
  window.SetLogs(SampleLogs());
  window.Show(UiPage::Overview);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0 && window.IsVisible()) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}

// The tests in the real window need a desktop: ctest runs them in CI (CI is
// set) or with SOVEREIGN_DESKTOP_TESTS=1, else they say they're skipped.
bool OnDesktop(const char* test) {
  std::array<wchar_t, 8> flag{};
  if (GetEnvironmentVariableW(L"CI", flag.data(), static_cast<DWORD>(flag.size())) == 0 &&
      GetEnvironmentVariableW(L"SOVEREIGN_DESKTOP_TESTS", flag.data(), static_cast<DWORD>(flag.size())) == 0) {
    std::printf("%s: skipped (set SOVEREIGN_DESKTOP_TESTS=1; CI runs it)\n", test);
    return false;
  }
  return true;
}

// This thread's own window - never FindWindowW: an installed Sovereign has
// a window of the same class, and the keys sent went to it (16.10.2026:
// a desktop run turned the user's connection off).
HWND OwnWindow() {
  HWND hwnd = nullptr;
  EnumThreadWindows(
      GetCurrentThreadId(),
      [](HWND candidate, LPARAM found) -> BOOL {
        std::array<wchar_t, 64> name{};
        GetClassNameW(candidate, name.data(), static_cast<int>(name.size()));
        if (std::wstring_view(name.data()) != L"SovereignMainWindow") {
          return TRUE;
        }
        *reinterpret_cast<HWND*>(found) = candidate;  // NOLINT(performance-no-int-to-ptr) - the API's context
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&hwnd));
  return hwnd;
}

// A fifth of a second of the window's messages. Timed, not drained: on the
// main page the live traffic's WM_PAINT asks for the next frame (0.5.3 on)
// and waits for vsync, so the queue never runs dry - draining it after Esc
// led there never returned (CI failed from dc9e160 on, a desktop run hung).
void Pump() {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (std::chrono::steady_clock::now() < until) {
    MSG msg;
    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    } else {
      Sleep(10);
    }
  }
}

// The window's pixel at `p` (client pixels) as its last frame put it there:
// DWM's copy of the window. Not the screen's: a dimmer over everything
// (PangoBright here, 23.10.2026) darkens that whatever the window drew.
std::optional<COLORREF> ShownPixel(HWND hwnd, POINT p) {
  RECT client{};
  GetClientRect(hwnd, &client);
  const wil::unique_hdc_window screen = wil::GetDC(hwnd);
  const wil::unique_hdc memory(CreateCompatibleDC(screen.get()));
  const wil::unique_hbitmap bitmap(CreateCompatibleBitmap(screen.get(), client.right, client.bottom));
  if (!memory || !bitmap) {
    return std::nullopt;
  }
  const auto selected = wil::SelectObject(memory.get(), bitmap.get());
  if (PrintWindow(hwnd, memory.get(), PW_CLIENTONLY | PW_RENDERFULLCONTENT) == FALSE) {
    return std::nullopt;
  }
  return GetPixel(memory.get(), p.x, p.y);
}

// --edit-test: typing a configuration's name in place, in the real window,
// driven by keys as a user would - Tab to the first configuration, Enter
// opens it, Tab to its title, Enter starts the edit box over it; Enter keeps
// what was typed, Esc drops it, clicking away keeps it. Needs a desktop.
int EditTest(HINSTANCE instance) {
  if (!OnDesktop("edit in place")) {
    return 0;
  }
  sovereign::tray::UiCommand command{};
  sovereign::tray::UiArgs got;
  int commands = 0;
  sovereign::tray::MainWindow window(instance, [&](sovereign::tray::UiCommand c, const sovereign::tray::UiArgs& a) {
    command = c;
    got = a;
    ++commands;
  });
  window.Update(SampleContent());
  window.Show(UiPage::Subscription);
  const HWND hwnd = OwnWindow();
  if (hwnd == nullptr) {
    std::fputs("edit in place: no window of this test's own\n", stderr);
    return 1;
  }
  const auto key = [&](WPARAM vk) {
    SendMessageW(hwnd, WM_KEYDOWN, vk, 0);
    Pump();
  };
  // Tab goes through the window's own caption first (0.4.97 on) - then the
  // page's back arrow. Before, the count left them out: Enter landed on "+"
  // and its menu waited for a click (CI failed from dc9e160 on, a desktop run
  // hung). The configurations' list has the three ways to add (paste, QR, file), "hide" and the cross there, a
  // configuration's page "hide" and the cross (MainWindow's Caption); both
  // "report a problem" and the new version's key before them (0.5.52 on), the
  // sample having one found.
  constexpr int kListCaption = 7;
  constexpr int kProfileCaption = 4;
  const auto tabs = [&](int n) {
    for (int i = 0; i < n; ++i) {
      key(VK_TAB);
    }
  };
  const auto startEdit = [&] {
    key(VK_ESCAPE);  // to the list, if on the configuration's page
    window.Show(UiPage::Subscription);
    tabs(kListCaption + 2);  // past the caption and back: the first configuration
    key(VK_RETURN);
    tabs(kProfileCaption + 2);  // past the caption and back: the title
    key(VK_RETURN);
    return FindWindowExW(hwnd, nullptr, L"EDIT", nullptr);
  };
  int failures = 0;
  const auto check = [&](bool ok, const char* what) {
    if (!ok) {
      std::fprintf(stderr, "edit in place: %s\n", what);
      ++failures;
    }
  };

  HWND box = startEdit();
  check(box != nullptr, "no edit box over the title");
  if (box != nullptr) {
    std::array<wchar_t, 64> text{};
    GetWindowTextW(box, text.data(), static_cast<int>(text.size()));
    check(std::wstring(text.data()) == L"packetlab", "the box doesn't hold the name");
    SetWindowTextW(box, L"  Мой NL  ");
    SendMessageW(box, WM_KEYDOWN, VK_RETURN, 0);
    Pump();
    check(commands == 1 && command == sovereign::tray::UiCommand::RenameProfile && got.index == 0 &&
              got.text == L"Мой NL",
          "Enter didn't send the name, trimmed");
    check(FindWindowExW(hwnd, nullptr, L"EDIT", nullptr) == nullptr, "the box stayed after Enter");
  }
  box = startEdit();
  if (box != nullptr) {
    SetWindowTextW(box, L"dropped");
    SendMessageW(box, WM_KEYDOWN, VK_ESCAPE, 0);
    Pump();
    check(commands == 1, "Esc sent something");
  }
  box = startEdit();
  if (box != nullptr) {
    SetWindowTextW(box, L"kept");
    SetFocus(hwnd);  // a click elsewhere in the window
    Pump();
    check(commands == 2 && got.text == L"kept", "leaving the box didn't keep the name");
  }
  window.Hide();
  return failures == 0 ? 0 : 1;
}

// --tip-test: the pointer rests on the title bar's report key; after the wait
// its tip (a dark box under the key) is in the window's frame.
int TipTest(HINSTANCE instance) {
  if (!OnDesktop("tip")) {
    return 0;
  }
  sovereign::tray::MainWindow window(instance, [](sovereign::tray::UiCommand, const sovereign::tray::UiArgs&) {});
  const UiContent content = SampleContent();
  window.Update(content);
  window.Show(UiPage::Overview);
  const HWND hwnd = OwnWindow();
  if (hwnd == nullptr) {
    return 1;
  }
  Pump();
  RECT client{};
  GetClientRect(hwnd, &client);
  const float scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
  const std::wstring text = sovereign::tray::DescribeMainWindowLayout(
      content, UiPage::Overview, static_cast<float>(client.right) / scale, static_cast<float>(client.bottom) / scale);
  const std::size_t found = text.find(L"Сообщить о проблеме");
  const std::size_t open = text.rfind(L'[', found);
  std::array<float, 4> r{};
  if (found == std::wstring::npos || swscanf_s(text.c_str() + open + 1, L"%f %f %f %f", &r[0], &r[1], &r[2], &r[3]) != 4) {
    std::fputs("tip: no report key in the layout\n", stderr);
    return 1;
  }
  const POINT centre{std::lround((r[0] + r[2]) / 2 * scale), std::lround((r[1] + r[3]) / 2 * scale)};
  // The cursor really there: else Windows says the mouse left (TrackMouseEvent) at once.
  POINT onScreen = centre;
  ClientToScreen(hwnd, &onScreen);
  SetCursorPos(onScreen.x, onScreen.y);
  SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(centre.x, centre.y));
  for (int i = 0; i < 4; ++i) {  // the wait is half a second; a second and the pointer is said to have left
    Pump();
  }
  int failures = 0;
  for (const int dy : {10, 16, 22}) {
    const POINT p{centre.x, std::lround(r[3] * scale) + std::lround(static_cast<float>(dy) * scale)};
    const std::optional<COLORREF> c = ShownPixel(hwnd, p);
    std::printf("tip: %d under the key: %d %d %d\n", dy, c ? GetRValue(*c) : -1, c ? GetGValue(*c) : -1,
                c ? GetBValue(*c) : -1);
    if (!c || GetRValue(*c) != 44 || GetBValue(*c) != 49) {
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}

// --ask-test: a question over the real window (MainWindow::Ask), answered as
// a user would. The page under it takes no click; Esc and "Отмена" drop it,
// Enter and its yes button answer it, Tab stays on its buttons; hiding the
// window drops it. The clicks are the window's mouse messages at the items'
// places in its layout (DescribeMainWindowLayout). Needs a desktop.
int AskTest(HINSTANCE instance) {
  if (!OnDesktop("ask")) {
    return 0;
  }
  int commands = 0;
  sovereign::tray::MainWindow window(instance,
                                     [&](sovereign::tray::UiCommand, const sovereign::tray::UiArgs&) { ++commands; });
  const UiContent content = SampleContent();
  window.Update(content);
  window.Show(UiPage::Overview);
  const HWND hwnd = OwnWindow();
  if (hwnd == nullptr) {
    std::fputs("ask: no window of this test's own\n", stderr);
    return 1;
  }
  Pump();
  const UiQuestion question{.title = L"Закрыть Sovereign?",
                            .text = L"Подключение останется: им управляет служба.",
                            .yes = L"Закрыть",
                            .tone = UiQuestion::Tone::Danger};
  UiContent asked = content;
  asked.question = question;
  // The centre of the last item of `kind` with `label` (any, if empty) in the
  // window's pixels: a question's buttons come last.
  RECT client{};
  GetClientRect(hwnd, &client);
  const float scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
  const auto boxOf = [&](const UiContent& c, std::wstring_view kind, std::wstring_view label) -> std::optional<RECT> {
    const std::wstring text = sovereign::tray::DescribeMainWindowLayout(
        c, UiPage::Overview, static_cast<float>(client.right) / scale, static_cast<float>(client.bottom) / scale);
    std::optional<RECT> found;
    for (std::size_t from = 0; from < text.size();) {
      const std::size_t end = std::min(text.find(L'\n', from), text.size());
      const std::wstring_view line(text.data() + from, end - from);
      from = end + 1;
      // "    Kind         [ left top right bottom] text  | detail" (four flags, a space)
      const std::size_t open = line.find(L'[');
      const std::size_t close = line.find(L"] ");
      if (open == std::wstring_view::npos || close == std::wstring_view::npos || line.size() < 5 + kind.size() ||
          line.substr(5, kind.size()) != kind || line[5 + kind.size()] != L' ') {
        continue;
      }
      const std::wstring_view rest = line.substr(close + 2);
      if (!label.empty() && rest != label && !rest.starts_with(std::wstring(label) + L"  |")) {
        continue;
      }
      std::array<float, 4> r{};
      if (swscanf_s(std::wstring(line.substr(open + 1, close - open - 1)).c_str(), L"%f %f %f %f", &r[0], &r[1], &r[2],
                    &r[3]) == 4) {
        found = RECT{std::lround(r[0] * scale), std::lround(r[1] * scale), std::lround(r[2] * scale),
                     std::lround(r[3] * scale)};
      }
    }
    return found;
  };
  const auto at = [&](const UiContent& c, std::wstring_view kind, std::wstring_view label) -> std::optional<POINT> {
    const std::optional<RECT> r = boxOf(c, kind, label);
    return r ? std::optional(POINT{(r->left + r->right) / 2, (r->top + r->bottom) / 2}) : std::nullopt;
  };
  const auto click = [&](POINT p) {
    const LPARAM where = MAKELPARAM(p.x, p.y);
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, where);
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, where);
    SendMessageW(hwnd, WM_LBUTTONUP, 0, where);
    Pump();
  };
  const auto key = [&](WPARAM vk) {
    SendMessageW(hwnd, WM_KEYDOWN, vk, 0);
    Pump();
  };
  int failures = 0;
  const auto check = [&](bool ok, const char* what) {
    if (!ok) {
      std::fprintf(stderr, "ask: %s\n", what);
      ++failures;
    }
  };
  const std::optional<POINT> power = at(content, L"Power", L"");
  const std::optional<POINT> yesButton = at(asked, L"Danger", question.yes);
  const std::optional<POINT> noButton = at(asked, L"Button", question.no);
  if (!power || !yesButton || !noButton) {
    std::fputs("ask: no power button or no question's buttons in the layout\n", stderr);
    return 1;
  }
  int yes = 0;
  const auto ask = [&] {
    window.Ask(question, [&] { ++yes; });
    Pump();
  };
  // The power button takes the click again: the question is gone.
  const auto gone = [&](const char* what) {
    const int before = commands;
    click(*power);
    check(commands == before + 1, what);
  };

  // Come in for good, its yes button as bright as on a still snapshot.
  for (const UiPage page : {UiPage::Servers, UiPage::Overview}) {
    window.Show(page);
    Pump();
    ask();
    Pump();
    if (const std::optional<RECT> r = boxOf(asked, L"Danger", question.yes)) {
      const std::optional<COLORREF> c =
          ShownPixel(hwnd, POINT{r->left + std::lround(6 * scale), (r->top + r->bottom) / 2});
      // The question's yes is the hand's, white (law.md, Button): kLit 226 229 235; the tone is the lamp in the head.
      check(c && std::abs(GetRValue(*c) - 226) <= 12 && std::abs(GetGValue(*c) - 229) <= 12 &&
                std::abs(GetBValue(*c) - 235) <= 12,
            "its yes button isn't kLit in the window's frame: the card came in dimmed");
    }
    key(VK_ESCAPE);
  }
  ask();
  click(*power);
  check(commands == 0, "the page under the question took a click");
  key(VK_ESCAPE);
  check(yes == 0, "Esc answered yes");
  gone("the question stayed after Esc");
  ask();
  key(VK_TAB);  // its yes, then
  key(VK_TAB);  // "Отмена" - its buttons only, left to right
  key(VK_TAB);  // and round to its yes...
  key(VK_TAB);  // ...and "Отмена" again, not the page's
  key(VK_RETURN);
  check(commands == 1 && yes == 0, "Tab went past the question's buttons or not in their order");
  gone("the question stayed after Enter on \"Отмена\"");
  ask();
  key(VK_RETURN);
  check(yes == 1, "Enter didn't answer yes");
  gone("the question stayed after Enter");
  ask();
  click(*noButton);
  check(yes == 1, "\"Отмена\" answered yes");
  gone("the question stayed after \"Отмена\"");
  ask();
  click(*yesButton);
  check(yes == 2, "its yes button didn't answer yes");
  gone("the question stayed after its yes button");
  ask();
  window.Hide();
  Pump();
  window.Show(UiPage::Overview);
  Pump();
  check(yes == 2, "hiding the window answered yes");
  gone("the question stayed over the window hidden and shown again");
  window.Hide();
  return failures == 0 ? 0 : 1;
}

// Every page laid out with the sample content at the default width and the
// narrowest, written as text into `dir` (DescribeMainWindowLayout): fails
// on clickable items that overlap or anything past the window's edge, and
// counts the lines whose text is cut.
int LayoutTest(const std::wstring& dir) {
  UiContent content = SampleContent();
  content.checks = SampleChecks();
  // The variants live on the heap: eight UiContents on the stack pass /analyze's 16 KB (C6262).
  std::vector<UiContent> variants(7, content);
  UiContent& routed = variants[0];
  // WARP on, with a site of its own: its card lists it, "Свои правила" doesn't.
  routed.routing.rules = {{L"qwen.ai, qwenlm.ai, alicdn.com, aliyun.com", L"напрямую"},
                          {L"chatgpt.com, openai.com", L"через WARP", true}};
  routed.routing.warp = true;
  routed.routing.warpRegistered = true;
  routed.routing.services = {{.name = L"Netflix", .way = L"через WARP"}, {.name = L"ChatGPT", .way = L"через WARP"}};
  routed.routing.board = SampleBoard();
  // The fine settings open.
  routed.routingMore = true;
  routed.routing.remoteDns = L"авто · Cloudflare";
  routed.routing.localDns = L"свой · dns.adguard-dns.com";
  // A question open over the overview (MainWindow::Ask), its title long enough to wrap.
  UiContent& asked = variants[1];
  asked.question = UiQuestion{.title = L"Как скачать подписку?",
                              .text = L"Подключения нет. Напрямую сервер подписки увидит твой настоящий IP-адрес. Через сервер "
                                      L"Sovereign (Cloudflare) он увидит адрес Cloudflare, но ссылку на подписку увидит тот, "
                                      L"кто держит этот сервер, — автор Sovereign. Выбор запомнится: Настройки → "
                                      L"«Скачивать без подключения».",
                              .yes = L"Через сервер Sovereign",
                              .no = L"Не добавлять",
                              .other = L"Напрямую",
                              .tone = UiQuestion::Tone::Warning,
                              .glyph = L"\xE896"};
  // A date asked on a calendar (MainWindow::AskDate), a month of 31 days that
  // starts late in the week, the three buttons in one row.
  UiContent& dated = variants[2];
  dated.question = UiQuestion{.title = L"Оплачено до",
                              .text = {},
                              .yes = L"Готово",
                              .other = L"Не следить",
                              .calendar = UiCalendar{.year = 2026, .month = 12, .day = 15,
                                                     .todayYear = 2026, .todayMonth = 12, .todayDay = 3},
                              .glyph = L"\xE787"};
  // The relay fold open, Sovereign's own chosen.
  UiContent& relayOpen = variants[3];
  relayOpen.settingsMore = true;
  relayOpen.sharedRelay = true;
  // Updates installed unasked, the longest of the row's values, one waiting
  // for a quiet moment; and only by the button, the longest idle status.
  UiContent& installing = variants[4];
  installing.updateMode = sovereign::tray::UpdateMode::Install;
  installing.update = sovereign::tray::UiUpdate::Waiting;
  installing.updateVersion = L"0.12.41";
  UiContent& idle = variants[5];
  idle.updateMode = sovereign::tray::UpdateMode::Off;
  idle.update = sovereign::tray::UiUpdate::Idle;
  idle.boxIsLatest = true;
  // A new version in the bar with the longest number, and things wrong: the
  // main screen's one line, the log's red paragraphs.
  UiContent& troubled = variants[6];
  troubled = installing;
  troubled.update = sovereign::tray::UiUpdate::Available;
  troubled.display = sovereign::tray::Display::ServiceDown;
  troubled.killSwitch = true;
  troubled.killSwitchError = L"не удалось добавить фильтр WFP";
  troubled.localNetwork = L"Тормозит твоя сеть, а не VPN: напрямую до интернета 900 мс.";
  struct Page {
    UiPage page;
    const wchar_t* name;
    const UiContent* content;
  };
  const std::array<Page, 15> pages = {{{UiPage::Overview, L"overview", &content},
                                      {UiPage::Overview, L"overview-troubled", &troubled},
                                      {UiPage::Logs, L"logs-troubled", &troubled},
                                      {UiPage::Servers, L"servers", &content},
                                      {UiPage::Subscription, L"subscription", &content},
                                      {UiPage::Logs, L"logs", &content},
                                      {UiPage::Settings, L"settings", &content},
                                      {UiPage::Profile, L"profile", &content},
                                      {UiPage::Routing, L"routing", &routed},
                                      {UiPage::Checks, L"checks", &content},
                                      {UiPage::Overview, L"question", &asked},
                                      {UiPage::Profile, L"question-date", &dated},
                                      {UiPage::Subscription, L"relay-open", &relayOpen},
                                      {UiPage::Settings, L"settings-install", &installing},
                                      {UiPage::Settings, L"settings-idle", &idle}}};
  CreateDirectoryW(dir.c_str(), nullptr);
  int failures = 0;
  int cut = 0;
  for (const float width : {420.0f, 360.0f}) {
    for (const auto& [page, name, sample] : pages) {
      const std::wstring text = sovereign::tray::DescribeMainWindowLayout(*sample, page, width, 660);
      std::string utf8(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                                                    static_cast<int>(text.size()), nullptr, 0,
                                                                    nullptr, nullptr)),
                       '\0');
      WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(),
                          static_cast<int>(utf8.size()), nullptr, nullptr);
      std::ofstream(dir + L"\\" + name + L"-" + std::to_wstring(static_cast<int>(width)) + L".txt",
                    std::ios::binary)
          << utf8;
      for (std::size_t at = 0; at < text.size();) {
        const std::size_t end = std::min(text.find(L'\n', at), text.size());
        const std::wstring_view line(text.data() + at, end - at);
        if (line.size() > 4 && (line[1] == L'x' || line[2] == L'>' || line[3] == L'e')) {
          std::fwprintf(stderr, L"%ls @%d: %.*ls\n", name, static_cast<int>(width), static_cast<int>(line.size()),
                        line.data());
          ++failures;
        }
        cut += line.size() > 4 && line[0] == L'!' ? 1 : 0;
        at = end + 1;
      }
    }
  }
  std::fwprintf(stdout, L"layout: %d overlapping or outside, %d lines of text cut\n", failures, cut);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    return 3;
  }
  int result = 2;
  try {
    const std::wstring arg = argc > 1 ? argv[1] : L"ui-snapshots";
    result = arg == L"--window"      ? Window(GetModuleHandleW(nullptr))
             : arg == L"--edit-test" ? EditTest(GetModuleHandleW(nullptr))
             : arg == L"--ask-test"  ? AskTest(GetModuleHandleW(nullptr))
             : arg == L"--tip-test"  ? TipTest(GetModuleHandleW(nullptr))
             : arg == L"--layout-test" ? LayoutTest(argc > 2 ? argv[2] : L"ui-layout")
                                     : Snapshots(arg);
  } catch (const wil::ResultException& e) {
    // what(): where it was thrown, the file and line.
    std::fprintf(stderr, "ui-snapshot: 0x%08lx %s\n", static_cast<unsigned long>(e.GetErrorCode()), e.what());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ui-snapshot: %s\n", e.what());
  }
  CoUninitialize();
  return result;
}
