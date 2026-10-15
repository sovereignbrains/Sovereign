#include "tray_model.h"

#include <algorithm>

namespace sovereign::tray {

FlowWay FlowWayOf(std::string_view tag) {
  const auto numbered = [&](std::string_view name) {
    // The tag itself, or it numbered apart from a server of the same name: "direct 2".
    return tag == name || (tag.starts_with(name) && tag.size() > name.size() + 1 && tag[name.size()] == ' ' &&
                           std::all_of(tag.begin() + static_cast<std::ptrdiff_t>(name.size()) + 1, tag.end(),
                                       [](char c) { return c >= '0' && c <= '9'; }));
  };
  if (tag.empty()) {
    return FlowWay::None;
  }
  if (numbered("direct")) {
    return FlowWay::Direct;
  }
  if (numbered("warp")) {
    return FlowWay::Warp;
  }
  return FlowWay::Vpn;
}

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
    if (!downSince_) {
      downSince_ = now;
    }
    downLong_ = now - *downSince_ >= kServiceGrace;
    last_.reset();
    lastAt_.reset();
    downRate_ = upRate_ = 0;
    flowRates_.fill(0);
    return Action::None;
  }
  serviceUp_ = true;
  downSince_.reset();
  downLong_ = false;

  // A rate needs two samples of the same running box: a new generation means
  // the counters restarted from zero.
  if (last_ && lastAt_ && last_->running && stats->running && last_->generation == stats->generation) {
    const double seconds = std::chrono::duration<double>(now - *lastAt_).count();
    if (seconds > 0) {
      downRate_ = std::max(0.0, static_cast<double>(stats->downlinkBytes - last_->downlinkBytes) / seconds);
      upRate_ = std::max(0.0, static_cast<double>(stats->uplinkBytes - last_->uplinkBytes) / seconds);
      // By stream: a total that dropped (the oldest closed connections fall
      // out of the engine's count) is nothing, not a negative.
      flowRates_.fill(0);
      for (const auto& [tag, total] : stats->flows) {
        const auto was = last_->flows.find(tag);
        const std::int64_t before = was != last_->flows.end() ? was->second : 0;
        flowRates_[static_cast<std::size_t>(FlowWayOf(tag))] +=
            std::max(0.0, static_cast<double>(total - before) / seconds);
      }
    }
  } else {
    downRate_ = upRate_ = 0;
    flowRates_.fill(0);
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
  flowRates_.fill(0);
}

Display TrayModel::GetDisplay() const {
  if (!serviceUp_) {
    if (downLong_) {
      return Display::ServiceDown;
    }
    return wantOn_ ? Display::Starting : Display::Off;  // a few seconds away: an update, a restart
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
  if (!wantOn_ || !serviceUp_ || startInFlight_ || !last_) {
    return Action::None;
  }
  if (!last_->running) {
    if (!retryAt_ || now >= *retryAt_) {
      startInFlight_ = true;
      return Action::Start;
    }
    return Action::None;
  }
  if (!expectedConfig_.empty() && last_->configSha256 != expectedConfig_ && restartedFor_ != expectedConfig_) {
    restartedFor_ = expectedConfig_;
    return Action::Stop;  // the next poll starts it with the current config
  }
  return Action::None;
}

}  // namespace sovereign::tray
