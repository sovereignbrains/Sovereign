#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "boot_store.h"
#include "core.h"
#include "kill_switch.h"
#include "log_ring.h"
#include "self_update.h"

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
// Commands: ping (echo), box_ping, box_start {config, at_boot?}, box_stop, box_boot {at_boot},
// box_stats, box_logs {since}, box_urltest {tags, url?, timeout_ms?},
// box_delays, box_exitip {tag, refresh?}, box_route {targets},
// kill_switch {enabled, allow_lan?, lan_closed?, lan_allowed?},
// update_install {version, installer, session}.
class ControlHandler {
 public:
  // box_urltest's limits: what one test may ask of the core.
  static constexpr std::size_t kMaxUrlTestTags = 256;
  static constexpr std::size_t kMaxTagBytes = 256;
  static constexpr std::size_t kMaxUrlBytes = 2048;
  static constexpr std::int64_t kMinUrlTestTimeoutMs = 1000;
  static constexpr std::int64_t kMaxUrlTestTimeoutMs = 30000;
  static constexpr std::int64_t kDefaultUrlTestTimeoutMs = 5000;
  // sing-box's own default (common/urltest).
  static constexpr std::string_view kDefaultUrlTestUrl = "https://www.gstatic.com/generate_204";

  // box_route's limits: a name and its addresses (gocore/route.go keeps the
  // answer within one read of the tray's pipe buffer).
  static constexpr std::size_t kMaxRouteTargets = 32;
  static constexpr std::size_t kMaxRouteTargetBytes = 253;

  // A box_logs response is capped so it always fits one read of the tray's
  // pipe buffer; the rest is fetched with the returned `next`.
  static constexpr std::size_t kMaxLogsResponseBytes = 3584;

  // core may be null (DLL missing): core commands then answer an error and
  // everything else still works. lastConfig is the service's memory of the
  // last successful box_start (replayed on resume from sleep): set on
  // box_start, cleared on box_stop. killSwitch may be null too: kill_switch then
  // answers an error. What it holds on construction (persistent filters from
  // before a restart) is reported as active until the tray says otherwise.
  //
  // Threads: Handle() comes from one thread at a time (the pipe server serves
  // one client after another); ResumeAfterSleep() may come from another (the
  // SCM's power events) at any moment. The state both touch - lastConfig, the
  // kill switch's settings, the core's start/stop - is under one lock; the
  // long core calls (box_urltest, box_delays, box_exitip) run outside it, so a
  // resume never waits for a latency test.
  //
  // boot may be null too: box_start's at_boot is then ignored and box_boot
  // answers an error. With one, box_start {at_boot: true} keeps the config it
  // started for the next boot, {at_boot: false} drops it (no at_boot: as it
  // was); box_boot {at_boot} does the same for the running config - the
  // user's "connect at startup" switched while connected.
  //
  // updates may be null too: update_install then answers an error. It runs
  // outside the state lock - it downloads a checksum.
  ControlHandler(ICore* core, const LogRing& coreLog, std::optional<std::string>& lastConfig,
                 CommandObserver observer = {}, IKillSwitch* killSwitch = nullptr, IBootStore* boot = nullptr,
                 IUpdateRunner* updates = nullptr);

  // update_install's limit on the installer's path.
  static constexpr std::size_t kMaxInstallerPathBytes = 2048;

  std::string Handle(const std::string& request);

  struct ResumeOutcome {
    bool replayed = false;  // false: no core, or no config to replay
    std::string stopError;
    std::string startError;
  };

  // After a resume from sleep, from whatever thread reports it (the SCM's):
  // restart the box with the last started config - always restart rather than
  // probe, wintun's adapter and the routes aren't guaranteed to survive a
  // suspend. Under the state lock, so a box_start/box_stop/kill_switch in flight
  // finishes first and the replay sees its result. (Before, the power-event
  // thread read lastConfig while the pipe thread could be assigning it - a data
  // race on a std::string.)
  ResumeOutcome ResumeAfterSleep();

  struct BootOutcome {
    bool tried = false;  // false: nothing to start, or the tray came first
    std::string error;   // a failed try: worth another once the network is up
  };

  // At the service's start, from its own thread: the config kept for boot
  // started, if there is one and the tray hasn't started or stopped the box
  // since the service came up (its box_start/box_stop wins). The kill
  // switch's persistent filters stay: they were built over this config's
  // tunnel before the reboot.
  BootOutcome StartAtBoot();

 private:
  // Set by Dispatch for the observer.
  struct Outcome {
    std::string_view cmd = "invalid";
    std::string error;
  };

  std::string Dispatch(const std::string& request, Outcome& outcome);
  // The kill switch's rules for `settings` over the running config's tunnel.
  std::string ApplyKillSwitch(const KillSwitchSettings& settings);

  ICore* core_;
  const LogRing& coreLog_;
  std::optional<std::string>& lastConfig_;
  CommandObserver observer_;
  IKillSwitch* killSwitch_;
  IBootStore* boot_;
  IUpdateRunner* updates_;
  KillSwitchSettings killSettings_;  // as last applied
  bool killActive_ = false;          // filters in place
  bool bootDone_ = false;            // StartAtBoot started it, or the tray took over
  // Guards lastConfig_, killSettings_, killActive_, bootDone_, the boot store
  // and the core's start/stop sequence - between Handle(), ResumeAfterSleep()
  // and StartAtBoot() (see the constructor).
  std::mutex stateMutex_;
};

}  // namespace sovereign::service
