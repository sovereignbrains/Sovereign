#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "core.h"
#include "log_ring.h"

namespace sovereign::service {

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
  ControlHandler(ICore* core, const LogRing& coreLog, std::optional<std::string>& lastConfig);

  std::string Handle(const std::string& request);

 private:
  ICore* core_;
  const LogRing& coreLog_;
  std::optional<std::string>& lastConfig_;
};

}  // namespace sovereign::service
