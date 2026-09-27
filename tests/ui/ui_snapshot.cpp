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

#include <cmath>
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
  c.status = L"вкл · ↓ 2.4 МБ/с ↑ 310.2 КБ/с · соединений: 37";
  c.statusDot = RGB(40, 175, 80);
  c.on = true;
  c.down = 2.4 * 1024 * 1024;
  c.up = 310.2 * 1024;
  c.connections = 37;
  for (int i = 0; i < 120; ++i) {
    const double t = i / 6.0;
    const double down = (1.2 + std::sin(t) * 0.6 + (i > 90 ? 1.1 : 0.0)) * 1024 * 1024;
    const double up = (180 + std::cos(t * 1.7) * 90) * 1024;
    c.history.emplace_back(static_cast<float>(down), static_cast<float>(up));
  }
  c.subscription = L"27.09 19:20";
  c.hasSubscription = true;
  c.subscriptionHost = L"packetlab.tech";
  c.updateHours = 12;
  c.appsInclude = false;
  c.apps = {L"steam.exe", L"Telegram.exe", L"qbittorrent.exe", L"EpicGamesLauncher.exe"};
  c.protocols = {L"auto", L"AnyTLS · Нидерланды", L"AnyTLS · Финляндия", L"REALITY · Германия", L"REALITY · Польша"};
  c.protocol = 1;
  using State = sovereign::tray::UiDelay::State;
  c.delays = {{State::Ok, 48}, {State::Ok, 52}, {State::Ok, 310}, {State::Failed, 0}, {State::Pending, 0}};
  c.canTestDelays = true;
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
  constexpr const wchar_t* kNames[sovereign::tray::kUiPageCount] = {L"overview", L"protocol", L"subscription",
                                                                     L"apps",     L"logs",     L"settings"};
  CreateDirectoryW(dir.c_str(), nullptr);
  for (int p = 0; p < sovereign::tray::kUiPageCount; ++p) {
    const std::wstring path = dir + L"\\" + kNames[p] + L".png";
    sovereign::tray::RenderMainWindowSnapshot(content, static_cast<UiPage>(p), logs, 980, 660, 96, path);
  }
  sovereign::tray::RenderMainWindowSnapshot(content, UiPage::Overview, logs, 1470, 990, 144, dir + L"\\overview-150.png");

  // The states that look different: off with no subscription, and an error.
  UiContent empty;
  empty.display = sovereign::tray::Display::Off;
  empty.statusDot = RGB(90, 90, 90);
  empty.subscription = L"нет";
  empty.version = content.version;
  sovereign::tray::RenderMainWindowSnapshot(empty, UiPage::Overview, {}, 980, 660, 96, dir + L"\\overview-empty.png");
  UiContent failed = content;
  failed.display = sovereign::tray::Display::Error;
  failed.statusDot = RGB(215, 50, 50);
  failed.error = L"уже работает другой клиент sing-box с TUN (адаптер sing-tun) - выключи его, Sovereign подключится сам";
  failed.subscriptionError = L"сервер ответил 403";
  sovereign::tray::RenderMainWindowSnapshot(failed, UiPage::Overview, logs, 980, 660, 96, dir + L"\\overview-error.png");
  sovereign::tray::RenderMainWindowSnapshot(failed, UiPage::Subscription, logs, 980, 660, 96,
                                            dir + L"\\subscription-error.png");
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
    result = arg == L"--window" ? Window(GetModuleHandleW(nullptr)) : Snapshots(arg);
  } catch (const wil::ResultException& e) {
    std::fprintf(stderr, "ui-snapshot: 0x%08lx\n", static_cast<unsigned long>(e.GetErrorCode()));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ui-snapshot: %s\n", e.what());
  }
  CoUninitialize();
  return result;
}
