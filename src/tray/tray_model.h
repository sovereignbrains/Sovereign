#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace sovereign::tray {

// What the tray shows.
enum class Display : std::uint8_t {
  ServiceDown,  // the control pipe doesn't answer
  Off,          // the user turned it off
  Starting,     // turned on, box_start sent or about to be
  On,           // the box runs
  Error,        // turned on, but the last box_start failed (retried after a backoff)
};

// One box_stats answer (see ControlHandler in src/service/control.cpp).
struct Stats {
  bool running = false;
  std::int64_t uplinkBytes = 0;
  std::int64_t downlinkBytes = 0;
  std::int64_t connections = 0;
  std::int64_t generation = 0;
  std::string configSha256;  // of the config the box runs; empty if unknown
};

enum class Action : std::uint8_t { None, Start, Stop };

// The tray's decisions, apart from Win32 so they can be tested
// (tests/unit/tray_model_test.cpp). The worker thread feeds it a poll result
// every second and carries out the Action it returns.
//
// "On" is a standing intent, not a one-off command: while it holds and the
// service answers but the box isn't running - the service restarted, the
// machine rebooted and the tray just started, or the previous start failed -
// the model asks for a start again, failed starts no more often than
// kRetryAfter. "Off" stops the box once and then leaves it alone.
//
// While on, the box must run the tray's config: when the service reports a
// different config hash (config.json changed - a subscription refresh, or while
// the tray wasn't running) the model stops the box, and the next poll starts
// it with the current config. Once per expected config: if the hash still
// differs after that, it doesn't loop.
class TrayModel {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds kRetryAfter{15};

  explicit TrayModel(bool wantOn) : wantOn_(wantOn) {}

  // The user's toggle. Returns the action to carry out right away.
  Action SetWantOn(bool on, Clock::time_point now);

  // The hash of the config a start would send now (empty: none/unknown - no
  // enforcement). See Sha256Hex in src/common/sha256.h.
  void SetExpectedConfig(std::string sha256) { expectedConfig_ = std::move(sha256); }
  bool WantOn() const { return wantOn_; }

  // A box_stats result (nullopt: the service didn't answer). Returns the
  // action to carry out.
  Action OnPoll(const std::optional<Stats>& stats, Clock::time_point now);

  // Results of the actions the model asked for. An empty error is success.
  void OnStartResult(const std::string& error, Clock::time_point now);
  void OnStopResult(const std::string& error);

  Display GetDisplay() const;
  // Bytes per second over the last two polls of the same box generation;
  // zero until there are two.
  double DownRate() const { return downRate_; }
  double UpRate() const { return upRate_; }
  std::int64_t Connections() const { return last_ ? last_->connections : 0; }
  // The last start/stop failure, cleared by the next success.
  const std::string& LastError() const { return lastError_; }

 private:
  Action Decide(Clock::time_point now);

  bool wantOn_;
  bool serviceUp_ = false;
  bool startInFlight_ = false;
  std::optional<Clock::time_point> retryAt_;
  std::optional<Stats> last_;
  std::optional<Clock::time_point> lastAt_;
  double downRate_ = 0;
  double upRate_ = 0;
  std::string lastError_;
  std::string expectedConfig_;
  std::string restartedFor_;  // the expected config a mismatch restart was made for
};

}  // namespace sovereign::tray
