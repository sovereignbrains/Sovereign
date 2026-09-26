#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "core.h"
#include "log_ring.h"

namespace sovereign::service {

// What the service records about one control request (ETW, see trace.h).
// Deliberately never the request itself: box_start carries a full sing-box
// config with passwords and keys, and anything else is echoed back verbatim.
struct CommandRecord {
  // A known command name, "echo" for anything the handler just echoes back,
  // "invalid" for a request that isn't a JSON object - never a string taken
  // from the request as is.
  std::string_view cmd;
  std::size_t requestBytes = 0;
  // Empty on success, otherwise the error message the response carried.
  std::string_view error;
  std::chrono::microseconds duration{};
};

// Called once per handled request, on the thread that handled it. Must not
// throw: it runs after the response is built, inside the pipe server's loop.
using CommandObserver = std::function<void(const CommandRecord&)>;

// The control-pipe protocol: one JSON request in, one JSON response out.
// Kept apart from the pipe transport and from any concrete core so it can be
// exercised with a fake ICore (tests/unit/control_test.cpp).
//
// Commands: ping (echo), box_ping, box_start {config}, box_stop,
// box_stats, box_logs {since}.
class ControlHandler {
 public:
  // A box_logs response is capped so it always fits one read of the tray's
  // pipe buffer; the rest is fetched with the returned `next`.
  static constexpr std::size_t kMaxLogsResponseBytes = 3584;

  // core may be null (DLL missing): core commands then answer an error and
  // everything else still works. lastConfig is the service's memory of the
  // last successful box_start (replayed on resume from sleep): set on
  // box_start, cleared on box_stop.
  ControlHandler(ICore* core, const LogRing& coreLog, std::optional<std::string>& lastConfig,
                 CommandObserver observer = {});

  std::string Handle(const std::string& request);

 private:
  // Set by Dispatch for the observer.
  struct Outcome {
    std::string_view cmd = "invalid";
    std::string error;
  };

  std::string Dispatch(const std::string& request, Outcome& outcome);

  ICore* core_;
  const LogRing& coreLog_;
  std::optional<std::string>& lastConfig_;
  CommandObserver observer_;
};

}  // namespace sovereign::service
