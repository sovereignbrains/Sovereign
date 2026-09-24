// The control-pipe protocol driven through a fake ICore: proves the service
// talks to "a core", not to GoCore specifically, and pins down every command's
// request/response shape without a DLL, a pipe or a running sing-box.
#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "control.h"
#include "core.h"
#include "log_ring.h"

namespace {

using nlohmann::json;
using sovereign::service::ControlHandler;
using sovereign::service::CoreStats;
using sovereign::service::ICore;
using sovereign::service::LogLevel;
using sovereign::service::LogRing;
using sovereign::service::LogSink;

class FakeCore final : public ICore {
 public:
  std::string Ping() override { return "fake pong"; }

  std::string Start(const std::string& configJson) override {
    if (running) {
      return "already running";
    }
    if (!startError.empty()) {
      return startError;
    }
    running = true;
    startedWith = configJson;
    return {};
  }

  std::string Stop() override {
    running = false;
    return stopError;
  }

  CoreStats Stats() override {
    CoreStats result = stats;
    result.running = running;
    return result;
  }

  void SetLogSink(LogSink sink) override { installedSink = std::move(sink); }

  bool running = false;
  std::string startError;
  std::string stopError;
  std::string startedWith;
  CoreStats stats;
  LogSink installedSink;
};

json Send(ControlHandler& handler, const std::string& request) {
  return json::parse(handler.Handle(request));
}

bool IsError(const json& response, const std::string& message) {
  return response.value("cmd", "") == "error" && response.value("message", "") == message;
}

void TestMalformedRequests() {
  LogRing log(10);
  std::optional<std::string> lastConfig;
  FakeCore core;
  ControlHandler handler(&core, log, lastConfig);

  CHECK(IsError(Send(handler, "{not json"), "invalid json"));
  CHECK(IsError(Send(handler, "[1,2]"), "request must be a json object"));
  CHECK(IsError(Send(handler, R"({"cmd":"box_start"})"), "missing config field"));
  CHECK(IsError(Send(handler, R"({"cmd":"box_logs","since":-1})"),
                "since must be a non-negative integer"));
  CHECK(IsError(Send(handler, R"({"cmd":"box_logs","since":"5"})"),
                "since must be a non-negative integer"));
}

void TestEcho() {
  LogRing log(10);
  std::optional<std::string> lastConfig;
  ControlHandler handler(nullptr, log, lastConfig);
  const json response = Send(handler, R"({"cmd":"ping","from":"test"})");
  CHECK(response.value("cmd", "") == "pong");
  CHECK(response["echo"]["from"] == "test");
}

void TestNoCoreLoaded() {
  LogRing log(10);
  log.Append(LogLevel::Info, "service-side line");
  std::optional<std::string> lastConfig;
  ControlHandler handler(nullptr, log, lastConfig);

  for (const char* cmd : {"box_ping", "box_stop", "box_stats"}) {
    CHECK(IsError(Send(handler, json{{"cmd", cmd}}.dump()), "gocore not loaded"));
  }
  CHECK(IsError(Send(handler, R"({"cmd":"box_start","config":{}})"), "gocore not loaded"));
  // The log ring belongs to the service and keeps answering without a core.
  const json logs = Send(handler, R"({"cmd":"box_logs"})");
  CHECK(logs.value("cmd", "") == "box_logs" && logs["entries"].size() == 1);
}

void TestLifecycleAndLastConfig() {
  LogRing log(10);
  std::optional<std::string> lastConfig;
  FakeCore core;
  ControlHandler handler(&core, log, lastConfig);

  CHECK(Send(handler, R"({"cmd":"box_ping"})")["reply"] == "fake pong");

  const json started =
      Send(handler, R"({"cmd":"box_start","config":{"outbounds":[{"type":"direct"}]}})");
  CHECK(started.value("cmd", "") == "box_started");
  CHECK(json::parse(core.startedWith) == json::parse(R"({"outbounds":[{"type":"direct"}]})"));
  CHECK(lastConfig.has_value() && *lastConfig == core.startedWith);

  // A failed start must not overwrite the remembered config.
  const json again = Send(handler, R"({"cmd":"box_start","config":{"log":{}}})");
  CHECK(IsError(again, "already running"));
  CHECK(lastConfig.has_value() && *lastConfig == core.startedWith);

  core.stats.uplinkBytes = 1234;
  core.stats.downlinkBytes = 5678;
  core.stats.activeConnections = 3;
  core.stats.generation = 7;
  const json stats = Send(handler, R"({"cmd":"box_stats"})");
  CHECK(stats.value("cmd", "") == "box_stats");
  CHECK(stats["running"] == true);
  CHECK(stats["uplink"] == 1234 && stats["downlink"] == 5678);
  CHECK(stats["connections"] == 3 && stats["generation"] == 7);

  CHECK(Send(handler, R"({"cmd":"box_stop"})").value("cmd", "") == "box_stopped");
  CHECK(!lastConfig.has_value());
  CHECK(Send(handler, R"({"cmd":"box_stats"})")["running"] == false);

  // A failing stop keeps the config: the box may well still be running.
  core.running = true;
  lastConfig = "{}";
  core.stopError = "cannot stop";
  CHECK(IsError(Send(handler, R"({"cmd":"box_stop"})"), "cannot stop"));
  CHECK(lastConfig.has_value());
}

void TestLogsPaging() {
  LogRing log(1000);
  std::optional<std::string> lastConfig;
  ControlHandler handler(nullptr, log, lastConfig);

  const json empty = Send(handler, R"({"cmd":"box_logs","since":0})");
  CHECK(empty["entries"].empty() && empty["next"] == 0 && empty["more"] == false);

  constexpr int kLines = 200;
  for (int i = 1; i <= kLines; ++i) {
    log.Append(i % 2 == 0 ? LogLevel::Warn : LogLevel::Info,
               "line " + std::to_string(i) + " " + std::string(150, 'x'));
  }

  // Page through with `next` until done: every line exactly once, in order,
  // and every response small enough for one tray pipe read.
  std::uint64_t since = 0;
  std::vector<std::uint64_t> seen;
  for (int page = 0; page < 100; ++page) {
    const std::string raw =
        handler.Handle(json{{"cmd", "box_logs"}, {"since", since}}.dump());
    CHECK(raw.size() <= ControlHandler::kMaxLogsResponseBytes);
    const json response = json::parse(raw);
    for (const json& entry : response["entries"]) {
      seen.push_back(entry["seq"].get<std::uint64_t>());
    }
    since = response["next"].get<std::uint64_t>();
    if (response["more"] == false) {
      break;
    }
  }
  CHECK(seen.size() == kLines);
  for (std::size_t i = 0; i < seen.size(); ++i) {
    CHECK(seen[i] == i + 1);
  }
  CHECK(since == kLines);

  const json firstPage = Send(handler, R"({"cmd":"box_logs","since":0})");
  CHECK(firstPage["entries"][0]["level"] == "info");
  CHECK(firstPage["entries"][1]["level"] == "warn");
}

void TestLogsSurviveBadUtf8() {
  LogRing log(10);
  log.Append(LogLevel::Error, "bad \xFF\xFE bytes from the network");
  std::optional<std::string> lastConfig;
  ControlHandler handler(nullptr, log, lastConfig);
  const json response = Send(handler, R"({"cmd":"box_logs"})");
  CHECK(response.value("cmd", "") == "box_logs" && response["entries"].size() == 1);
}

void TestReaderBehindRingDrop() {
  LogRing log(2);
  for (int i = 0; i < 5; ++i) {
    log.Append(LogLevel::Info, "x");
  }
  std::optional<std::string> lastConfig;
  ControlHandler handler(nullptr, log, lastConfig);
  // Lines 1-3 were dropped: a reader at 1 gets what survived, not an error.
  const json response = Send(handler, R"({"cmd":"box_logs","since":1})");
  CHECK(response["entries"].size() == 2 && response["next"] == 5);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) — see the catch below
  try {
    TestMalformedRequests();
    TestEcho();
    TestNoCoreLoaded();
    TestLifecycleAndLastConfig();
    TestLogsPaging();
    TestLogsSurviveBadUtf8();
    TestReaderBehindRingDrop();
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  if (sovereign::test::Failures() != 0) {
    return 1;
  }
  std::cout << "OK\n";
  return 0;
}
