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

#include <array>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "main_window.h"
#include "ui_content.h"

namespace {

using sovereign::tray::UiContent;
using sovereign::tray::UiPage;

UiContent SampleContent() {
  UiContent c;
  c.display = sovereign::tray::Display::On;
  c.on = true;
  c.down = 2.4 * 1024 * 1024;
  c.up = 310.2 * 1024;
  c.connections = 37;
  c.hasConfig = true;
  using sovereign::tray::UiServer;
  sovereign::tray::UiProfile packetlab;
  packetlab.id = "p1";
  packetlab.name = L"packetlab";
  packetlab.detail = L"подписка · серверов: 3 (выкл. 1) · 27.09 19:20";
  packetlab.subscription = true;
  packetlab.host = L"packetlab.tech";
  packetlab.updated = L"27.09 19:20";
  packetlab.period = L"каждые 12 ч (как советует сервер)";
  packetlab.servers = {UiServer{L"AnyTLS · Нидерланды", L"AnyTLS · REALITY", true},
                       UiServer{L"AnyTLS · Финляндия", L"AnyTLS", true},
                       UiServer{L"REALITY · Германия", L"VLESS · REALITY", false}};
  sovereign::tray::UiProfile keys;
  keys.id = "p2";
  keys.name = L"NL-1 и ещё 2";
  keys.detail = L"свой конфиг · серверов: 3";
  keys.servers = {UiServer{L"NL-1", L"Trojan · WS", true}, UiServer{L"DE-2", L"Hysteria2", true},
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
  c.protocol = 1;
  using State = sovereign::tray::UiDelay::State;
  c.delays = {{State::Ok, 48}, {State::Ok, 52}, {State::Ok, 310}, {State::Failed, 0}, {State::Pending, 0}};
  c.canTestDelays = true;
  c.exitIp = L"185.12.34.56";
  c.exitCountry = L"NL";
  c.exitCountryName = L"Нидерланды";
  c.update = sovereign::tray::UiUpdate::Available;
  c.updateVersion = L"0.3.0";
  c.autostart = true;
  c.version = L"0.2.0 · sing-box 1.14.1";
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

int Snapshots(const std::wstring& dir) {
  const UiContent content = SampleContent();
  const std::vector<std::wstring> logs = SampleLogs();
  constexpr const wchar_t* kNames[sovereign::tray::kUiPageCount] = {L"overview", L"servers", L"subscription",
                                                                     L"apps",     L"logs",    L"settings"};
  CreateDirectoryW(dir.c_str(), nullptr);
  for (int p = 0; p < sovereign::tray::kUiPageCount; ++p) {
    const std::wstring path = dir + L"\\" + kNames[p] + L".png";
    sovereign::tray::RenderMainWindowSnapshot(content, static_cast<UiPage>(p), logs, 400, 620, 96, path);
  }
  sovereign::tray::RenderMainWindowSnapshot(content, UiPage::Overview, logs, 600, 930, 144, dir + L"\\overview-150.png");
  // The routing: own, with rules; and a configuration's.
  UiContent routed = content;
  routed.routing.remoteDns = L"Cloudflare (DoH)";
  routed.routing.localDns = L"Cloudflare (DoH)";
  routed.routing.rules = {{L"qwen.ai, qwenlm.ai, alicdn.com, aliyun.com", L"напрямую"},
                          {L"10.9.0.0/16, steam.exe", L"через прокси"},
                          {L"~tracker", L"блокировать"}};
  routed.routing.lists = L"Списки правил на месте, обновлены 01.10 14:00; обновляются раз в сутки.";
  sovereign::tray::RenderMainWindowSnapshot(routed, UiPage::Routing, logs, 400, 1400, 96, dir + L"\\routing.png");
  routed.routing.own = false;
  routed.routing.sourceProfile = "p1";
  routed.routing.lists = L"Списки правил не скачались: сервер ответил 503. Пока работают зоны .ru/.рф/.su.";
  routed.routing.listsFailed = true;
  sovereign::tray::RenderMainWindowSnapshot(routed, UiPage::Routing, logs, 400, 1100, 96,
                                            dir + L"\\routing-subscription.png");
  // The checks: run, one warning, one failing, one running, speed and an address.
  UiContent checked = content;
  using Check = sovereign::tray::UiCheck;
  checked.checks = {
      {L"Выход через прокси", L"5.83.147.210 · DE", L"Этот адрес видят зарубежные сайты.", Check::Status::Ok},
      {L"Российское напрямую", L"46.148.140.142", L"Российские сайты видят твой адрес, не адрес сервера.", Check::Status::Ok},
      {L"Утечка DNS", L"i3D.net B.V (DE)", L"Зарубежные имена спрашиваются через прокси - провайдер их не видит.",
       Check::Status::Ok},
      {L"WebRTC / UDP", L"UDP не проходит",
       L"STUN не ответил: звонки в браузере и игры через UDP могут не работать (утечки при этом нет).", Check::Status::Warn},
      {L"IPv6", L"2a01:db8::1 · RU", L"IPv6 уходит мимо прокси - сайты могут увидеть твой адрес.", Check::Status::Fail},
      {L"Локальная сеть", L"проверяю...", L"", Check::Status::Running},
      {L"Задержка DNS", L"", L"", Check::Status::NotRun},
      {L"Скорость интернета", L"↓ 151 · ↑ 98 Мбит/с · 84 мс", L"Через прокси, до ближайшего узла Cloudflare.",
       Check::Status::Ok},
      {L"Куда пойдёт адрес", L"vk.com -> напрямую", L"Ни одно из твоих правил не подошло - решили списки.",
       Check::Status::Ok}};
  checked.checksRunning = true;
  sovereign::tray::RenderMainWindowSnapshot(checked, UiPage::Checks, logs, 400, 1300, 96, dir + L"\\checks.png");
  // A configuration's page: the first one's.
  sovereign::tray::RenderMainWindowSnapshot(content, UiPage::Profile, logs, 400, 620, 96, dir + L"\\profile.png");

  // The states that look different: off with no subscription, and an error.
  UiContent empty;
  empty.display = sovereign::tray::Display::Off;
  empty.version = content.version;
  sovereign::tray::RenderMainWindowSnapshot(empty, UiPage::Overview, {}, 400, 620, 96, dir + L"\\overview-empty.png");
  UiContent failed = content;
  failed.display = sovereign::tray::Display::Error;
  failed.error = L"уже работает другой клиент sing-box с TUN (адаптер sing-tun) - выключи его, Sovereign подключится сам";
  failed.profiles[0].failed = true;
  failed.profiles[0].error = L"сервер ответил 403";
  sovereign::tray::RenderMainWindowSnapshot(failed, UiPage::Overview, logs, 400, 620, 96, dir + L"\\overview-error.png");
  sovereign::tray::RenderMainWindowSnapshot(failed, UiPage::Profile, logs, 400, 620, 96,
                                            dir + L"\\profile-error.png");
  // A drop with the kill switch on: the internet held closed, and the switch
  // saying so; in settings, both switches.
  UiContent held = content;
  held.display = sovereign::tray::Display::Error;
  held.error = L"ядро остановилось";
  held.killSwitch = true;
  held.killSwitchActive = true;
  sovereign::tray::RenderMainWindowSnapshot(held, UiPage::Overview, logs, 400, 620, 96, dir + L"\\overview-killswitch.png");
  held.killSwitchError = L"WFP: доступ запрещён";
  sovereign::tray::RenderMainWindowSnapshot(held, UiPage::Settings, logs, 400, 620, 96, dir + L"\\settings-killswitch.png");

  // A newer subscription over the user's edits: the choice; then, narrow,
  // the buttons stacked; and after a carry-over, where both sides met.
  UiContent waiting = content;
  waiting.profiles[0].edited = true;
  waiting.profiles[0].waiting = true;
  sovereign::tray::RenderMainWindowSnapshot(waiting, UiPage::Profile, logs, 400, 620, 96,
                                            dir + L"\\profile-waiting.png");
  sovereign::tray::RenderMainWindowSnapshot(waiting, UiPage::Overview, logs, 400, 620, 96,
                                            dir + L"\\overview-waiting.png");
  waiting.profiles[0].choiceError = L"после переноса правок конфиг не годится: в конфиге нет ни одного outbound";
  sovereign::tray::RenderMainWindowSnapshot(waiting, UiPage::Profile, logs, 360, 640, 96,
                                            dir + L"\\profile-waiting-narrow.png");
  // The exit's address hidden: the flag and the country.
  UiContent hidden = content;
  hidden.hideExitIp = true;
  sovereign::tray::RenderMainWindowSnapshot(hidden, UiPage::Overview, logs, 400, 620, 96,
                                            dir + L"\\overview-ip-hidden.png");

  UiContent merged = content;
  merged.profiles[0].edited = true;
  merged.profiles[0].mergeNotes = {L"route.rules", L"outbounds[nl].server_port"};
  sovereign::tray::RenderMainWindowSnapshot(merged, UiPage::Profile, logs, 400, 620, 96,
                                            dir + L"\\profile-merged.png");
  return 0;
}

int Window(HINSTANCE instance) {
  sovereign::tray::MainWindow window(instance, [](sovereign::tray::UiCommand, const sovereign::tray::UiArgs&) {});
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

// --edit-test: typing a configuration's name in place, in the real window,
// driven by keys as a user would - Tab to the first configuration, Enter
// opens it, Tab to its title, Enter starts the edit box over it; Enter keeps
// what was typed, Esc drops it, clicking away keeps it. Needs a desktop:
// ctest runs it in CI (CI is set) or with SOVEREIGN_DESKTOP_TESTS=1.
int EditTest(HINSTANCE instance) {
  std::array<wchar_t, 8> flag{};
  if (GetEnvironmentVariableW(L"CI", flag.data(), static_cast<DWORD>(flag.size())) == 0 &&
      GetEnvironmentVariableW(L"SOVEREIGN_DESKTOP_TESTS", flag.data(), static_cast<DWORD>(flag.size())) == 0) {
    std::puts("edit in place: skipped (set SOVEREIGN_DESKTOP_TESTS=1; CI runs it)");
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
  HWND hwnd = FindWindowW(L"SovereignMainWindow", nullptr);
  const auto pump = [] {
    for (int i = 0; i < 20; ++i) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      Sleep(10);
    }
  };
  const auto key = [&](WPARAM vk) {
    SendMessageW(hwnd, WM_KEYDOWN, vk, 0);
    pump();
  };
  const auto startEdit = [&] {
    key(VK_ESCAPE);  // to the list, if on the configuration's page
    window.Show(UiPage::Subscription);
    key(VK_TAB);  // back
    key(VK_TAB);  // the first configuration
    key(VK_RETURN);
    key(VK_TAB);  // back
    key(VK_TAB);  // the title
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
    pump();
    check(commands == 1 && command == sovereign::tray::UiCommand::RenameProfile && got.index == 0 &&
              got.text == L"Мой NL",
          "Enter didn't send the name, trimmed");
    check(FindWindowExW(hwnd, nullptr, L"EDIT", nullptr) == nullptr, "the box stayed after Enter");
  }
  box = startEdit();
  if (box != nullptr) {
    SetWindowTextW(box, L"dropped");
    SendMessageW(box, WM_KEYDOWN, VK_ESCAPE, 0);
    pump();
    check(commands == 1, "Esc sent something");
  }
  box = startEdit();
  if (box != nullptr) {
    SetWindowTextW(box, L"kept");
    SetFocus(hwnd);  // a click elsewhere in the window
    pump();
    check(commands == 2 && got.text == L"kept", "leaving the box didn't keep the name");
  }
  window.Hide();
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
                                     : Snapshots(arg);
  } catch (const wil::ResultException& e) {
    std::fprintf(stderr, "ui-snapshot: 0x%08lx\n", static_cast<unsigned long>(e.GetErrorCode()));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ui-snapshot: %s\n", e.what());
  }
  CoUninitialize();
  return result;
}
