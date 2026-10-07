#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

#include "diagnostics.h"
#include "routing.h"

// The checks themselves (diagnostics.h judges): the tray's own requests go
// through the system like any program's - into the TUN, through the rules -
// so what they meet is what the user's programs meet. Blocking: run on a
// thread of its own.

namespace sovereign::tray {

// In the order the page lists them.
enum class CheckId : std::uint8_t { Exit, RussiaDirect, DnsLeak, WebRtc, Ipv6, Lan, DnsLatency, Speed, Route };
inline constexpr std::size_t kCheckCount = 9;

struct DiagnoseInput {
  RoutingSettings routing;
  std::string host;  // Route: the address to look up (UTF-8)
  std::wstring userAgent;
  // The core's newest log lines (UTF-8): where Route reads the outbound.
  std::function<std::vector<std::string>()> logs;
  // The local network closed by the user (settings.h lanClosed, in force:
  // the connection on), and the addresses let in: Lan says the router not
  // answering is what was asked for - and answering, that the closing leaks.
  bool lanClosed = false;
  std::vector<std::string> lanAllowed;
};

// Each check as it starts (Running) and as it ends.
using DiagnoseProgress = std::function<void(CheckId, const CheckResult&)>;

void RunChecks(const std::stop_token& stop, const std::vector<CheckId>& which, const DiagnoseInput& input,
               const DiagnoseProgress& progress);

}  // namespace sovereign::tray
