// libFuzzer target: the service's control protocol - what any local user can
// send down the pipe to a service running as SYSTEM. Every request, however
// malformed, must get back exactly one JSON object with a "cmd", nothing may
// throw out of Handle(), and a box_logs answer must stay within the size the
// tray reads in one go. The core is a fake that accepts any config; the log
// ring holds a few lines, bad UTF-8 included.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "control.h"
#include "core.h"
#include "log_ring.h"

namespace {

using namespace sovereign::service;

class FakeCore final : public ICore {
 public:
  std::string Ping() override { return "fake"; }
  std::string Start(const std::string& /*configJson*/) override {
    running_ = !running_;  // alternate success and "already running"
    return running_ ? std::string{} : std::string("already running");
  }
  std::string Stop() override {
    running_ = false;
    return {};
  }
  CoreStats Stats() override {
    CoreStats stats;
    stats.running = running_;
    return stats;
  }
  std::string StartUrlTest(const UrlTestRequest& /*request*/) override {
    return running_ ? std::string{} : std::string("box not running");
  }
  std::vector<DelayResult> Delays() override {
    // Tags come from configs: any bytes, bad UTF-8 included.
    return {{"nl", DelayResult::State::Ok, 48, {}},
            {"bad \xFF tag", DelayResult::State::Failed, 0, "err \xFE"},
            {"de", DelayResult::State::Pending, 0, {}}};
  }
  ExitIp LookupExitIp(const std::string& tag, bool /*refresh*/) override {
    // What comes back from the core is as unchecked as a tag: any bytes.
    return {tag, false, "185.12.34.56", "N\xFF", "err \xFE"};
  }
  void SetLogSink(LogSink /*sink*/) override {}

 private:
  bool running_ = false;
};

// Takes any rules: a kill_switch request - and a box_start's config, whose
// tunnel addresses become rules - get as far as the filters.
class FakeKillSwitch final : public sovereign::service::IKillSwitch {
 public:
  std::string Apply(const std::vector<sovereign::service::KillSwitchRule>& rules) override {
    active_ = !rules.empty();
    return {};
  }
  bool Active() override { return active_; }

 private:
  bool active_ = false;
};

struct World {
  FakeCore core;
  FakeKillSwitch killSwitch;
  LogRing log{16};
  std::optional<std::string> lastConfig;
  ControlHandler handler{&core, log, lastConfig, {}, &killSwitch};

  World() {
    log.Append(LogLevel::Info, "started");
    log.Append(LogLevel::Error, "bad \xFF\xFE bytes from the network");
    log.Append(LogLevel::Warn, std::string(3000, 'x'));
  }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static World world;
  const std::string request(reinterpret_cast<const char*>(data), size);
  const std::string response = world.handler.Handle(request);
  const auto json = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object() || !json.contains("cmd") || !json["cmd"].is_string()) {
    std::abort();
  }
  if (json["cmd"] == "box_logs" && response.size() > ControlHandler::kMaxLogsResponseBytes) {
    std::abort();
  }
  return 0;
}
