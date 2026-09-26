#include "tray_model.h"

#include <algorithm>

namespace sovereign::tray {

Action TrayModel::SetWantOn(bool on, Clock::time_point now) {
  wantOn_ = on;
  retryAt_.reset();
  lastError_.clear();
  if (!on) {
    // Stop once; a box nobody runs is fine to stop again (box_stop succeeds).
    return serviceUp_ ? Action::Stop : Action::None;
  }
  return Decide(now);
}

Action TrayModel::OnPoll(const std::optional<Stats>& stats, Clock::time_point now) {
  if (!stats) {
    serviceUp_ = false;
    last_.reset();
    lastAt_.reset();
    downRate_ = upRate_ = 0;
    return Action::None;
  }
  serviceUp_ = true;

  // A rate needs two samples of the same running box: a new generation means
  // the counters restarted from zero.
  if (last_ && lastAt_ && last_->running && stats->running && last_->generation == stats->generation) {
    const double seconds = std::chrono::duration<double>(now - *lastAt_).count();
    if (seconds > 0) {
      downRate_ = std::max(0.0, static_cast<double>(stats->downlinkBytes - last_->downlinkBytes) / seconds);
      upRate_ = std::max(0.0, static_cast<double>(stats->uplinkBytes - last_->uplinkBytes) / seconds);
    }
  } else {
    downRate_ = upRate_ = 0;
  }
  last_ = stats;
  lastAt_ = now;

  if (wantOn_ && stats->running) {
    lastError_.clear();  // e.g. "already started": it runs, whoever started it
    retryAt_.reset();
  }
  return Decide(now);
}

void TrayModel::OnStartResult(const std::string& error, Clock::time_point now) {
  startInFlight_ = false;
  if (error.empty()) {
    lastError_.clear();
    retryAt_.reset();
    if (last_) {
      last_->running = true;  // shown right away; the next poll confirms
    }
    return;
  }
  lastError_ = error;
  retryAt_ = now + kRetryAfter;
}

void TrayModel::OnStopResult(const std::string& error) {
  if (!error.empty()) {
    lastError_ = error;
    return;
  }
  lastError_.clear();
  if (last_) {
    last_->running = false;
  }
  downRate_ = upRate_ = 0;
}

Display TrayModel::GetDisplay() const {
  if (!serviceUp_) {
    return Display::ServiceDown;
  }
  if (last_ && last_->running) {
    return Display::On;
  }
  if (!wantOn_) {
    return Display::Off;
  }
  if (!lastError_.empty() && !startInFlight_) {
    return Display::Error;
  }
  return Display::Starting;
}

Action TrayModel::Decide(Clock::time_point now) {
  if (wantOn_ && serviceUp_ && !startInFlight_ && last_ && !last_->running &&
      (!retryAt_ || now >= *retryAt_)) {
    startInFlight_ = true;
    return Action::Start;
  }
  return Action::None;
}

}  // namespace sovereign::tray
