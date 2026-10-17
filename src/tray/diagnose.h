#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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
  std::string host;  // Route: the address to look up (UTF-8); empty - kRouteProbes
  // Route: where the routing board sends `host` - "proxy", "direct", "warp",
  // "block" (routing.h's buckets); the result says whether it went so. Empty:
  // nothing to hold it to (an address typed by hand).
  std::string expect;
  std::wstring userAgent;
  // Where the newest connection to one of the addresses went (the service's
  // box_route): Route and Speed ask it after their requests.
  std::function<std::optional<RouteWay>(const std::vector<std::string>&)> routeOf;
  // The local network closed by the user (settings.h lanClosed, in force:
  // the connection on), and the addresses let in: Lan says the router not
  // answering is what was asked for - and answering, that the closing leaks.
  bool lanClosed = false;
  std::vector<std::string> lanAllowed;
  // The VPN's own exit, as the core finds it through the server in use
  // (box_exitip); empty while unknown. The checks' requests are the tray's:
  // with only chosen programs and sites through the VPN (`selective`: what
  // no rule names goes directly, or only the listed apps go through) they go
  // directly like anything unlisted - what they see is then the user's own
  // address, not the VPN's, and that is as meant (16.10.2026: the page took
  // it for the VPN's and called the home address "the VPN's exit").
  std::string vpnIp;
  std::string vpnCountry;
  bool selective = false;
};

// Each check as it starts (Running) and as it ends.
using DiagnoseProgress = std::function<void(CheckId, const CheckResult&)>;

void RunChecks(const std::stop_token& stop, const std::vector<CheckId>& which, const DiagnoseInput& input,
               const DiagnoseProgress& progress);

}  // namespace sovereign::tray
