// diagnose-probe: the "Проверка" page's checks from a console, on this
// machine as it is - for trying them out by hand, not a ctest (they go to
// the internet). `diagnose-probe [all|speed|route <host>]`.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <exception>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "diagnose.h"

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  using sovereign::tray::CheckId;
  SetConsoleOutputCP(CP_UTF8);
  try {
    const std::string_view what = argc > 1 ? argv[1] : "all";
    std::vector<CheckId> which = {CheckId::Exit, CheckId::RussiaDirect, CheckId::DnsLeak, CheckId::WebRtc,
                                  CheckId::Ipv6, CheckId::Lan,          CheckId::DnsLatency};
    sovereign::tray::DiagnoseInput input;
    input.userAgent = L"Sovereign diagnose-probe";
    if (what == "speed") {
      which = {CheckId::Speed};
    } else if (what == "route" && argc > 2) {
      which = {CheckId::Route};
      input.host = argv[2];
    }
    static constexpr const char* kNames[] = {"exit", "russia", "dns leak", "webrtc", "ipv6", "lan", "dns latency",
                                             "speed", "route"};
    const std::stop_source stop;
    sovereign::tray::RunChecks(stop.get_token(), which, input,
                               [](CheckId id, const sovereign::tray::CheckResult& r) {
                                 if (r.status == sovereign::tray::CheckResult::Status::Running) {
                                   return;
                                 }
                                 static constexpr const char* kStatus[] = {"-", "...", "OK  ", "WARN", "FAIL"};
                                 std::printf("%s %-12s %s\n     %s\n", kStatus[static_cast<int>(r.status)],
                                             kNames[static_cast<int>(id)], r.summary.c_str(), r.detail.c_str());
                               });
  } catch (const std::exception& e) {
    std::fprintf(stderr, "diagnose-probe: %s\n", e.what());
    return 1;
  }
  return 0;
}
