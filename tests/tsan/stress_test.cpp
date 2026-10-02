// ThreadSanitizer stress test (tests/tsan): the service's control plane with
// its threads racing each other, as they do in the service -
//   * pipe requests (one thread: the pipe server serves one client at a time,
//     and that is Handle's contract),
//   * resumes from sleep (the SCM's power-event thread),
//   * log lines from the core's own threads into the ring.
// The long core calls (box_urltest, box_delays, box_exitip) aren't sent: they
// go to the core outside the state lock on purpose, and the real core
// synchronizes them itself - a fake here would only be testing itself.
// The fake core has no synchronization of its own on purpose: everything that
// reaches it must come through ControlHandler's lock, so a path that doesn't
// is a TSan report. On top of that the test checks what must hold whatever
// the interleaving: every request gets a JSON answer with a "cmd", the ring's
// sequence numbers stay contiguous and increasing, none is lost.

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "control.h"
#include "core.h"
#include "log_ring.h"

namespace {

using namespace sovereign::service;

constexpr int kRounds = 2000;
constexpr int kLogThreads = 2;
constexpr int kLinesPerThread = 5000;

// Deliberately unsynchronized (see the file comment).
class UnsyncedCore final : public ICore {
 public:
  std::string Ping() override { return "pong"; }
  std::string Start(const std::string& configJson) override {
    if (running_) {
      return "already running";
    }
    running_ = true;
    config_ = configJson;
    ++starts_;
    return {};
  }
  std::string Stop() override {
    running_ = false;
    return {};
  }
  CoreStats Stats() override {
    CoreStats stats;
    stats.running = running_;
    stats.generation = starts_;
    return stats;
  }
  std::string StartUrlTest(const UrlTestRequest& /*request*/) override { return {}; }
  std::vector<DelayResult> Delays() override { return {}; }
  ExitIp LookupExitIp(const std::string& /*tag*/, bool /*refresh*/) override { return {}; }
  std::string Select(const std::string& /*selector*/, const std::string& /*outbound*/) override { return {}; }
  void SetLogSink(LogSink /*sink*/) override {}

 private:
  bool running_ = false;
  std::string config_;
  std::int64_t starts_ = 0;
};

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - a throw here is a test failure anyway
  UnsyncedCore core;
  LogRing log(512);
  std::optional<std::string> lastConfig;
  std::atomic<int> observed{0};
  ControlHandler handler(&core, log, lastConfig, [&observed](const CommandRecord&) { ++observed; });

  std::atomic<int> badAnswers{0};
  std::atomic<int> badPages{0};
  std::atomic<bool> writing{true};
  std::vector<std::thread> threads;
  threads.reserve(3);

  // The pipe.
  threads.emplace_back([&] {
    const std::string start = R"({"cmd":"box_start","config":{"n":1}})";
    for (int i = 0; i < kRounds; ++i) {
      for (const std::string& request :
           {start, std::string(R"({"cmd":"box_stats"})"),
            std::string(R"({"cmd":"box_logs","since":)") + std::to_string(i) + "}",
            std::string(R"({"cmd":"box_stop"})")}) {
        const auto answer = nlohmann::json::parse(handler.Handle(request), nullptr, false);
        if (!answer.is_object() || !answer.contains("cmd")) {
          ++badAnswers;
        }
      }
    }
  });

  // The SCM's power events.
  threads.emplace_back([&] {
    for (int i = 0; i < kRounds; ++i) {
      (void)handler.ResumeAfterSleep();
    }
  });

  // The core's log threads.
  std::vector<std::thread> writers;
  writers.reserve(kLogThreads);
  for (int w = 0; w < kLogThreads; ++w) {
    writers.emplace_back([&, w] {
      for (int i = 0; i < kLinesPerThread; ++i) {
        log.Append(i % 7 == 0 ? LogLevel::Error : LogLevel::Info,
                   "writer " + std::to_string(w) + " line " + std::to_string(i));
      }
    });
  }

  // A reader like box_logs, without the pipe: whatever it gets must be a
  // contiguous, increasing run of sequence numbers.
  threads.emplace_back([&] {
    std::uint64_t since = 0;
    while (writing.load()) {
      const auto entries = log.Since(since);
      for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].seq != entries[i - 1].seq + 1) {
          ++badPages;
        }
      }
      if (!entries.empty()) {
        if (entries.front().seq <= since) {
          ++badPages;
        }
        since = entries.back().seq;
      }
      (void)log.LastSeq();
    }
  });

  for (auto& w : writers) {
    w.join();
  }
  writing = false;
  for (auto& t : threads) {
    t.join();
  }

  CHECK(badAnswers.load() == 0);
  CHECK(badPages.load() == 0);
  CHECK(observed.load() == kRounds * 4);
  CHECK(log.LastSeq() == static_cast<std::uint64_t>(kLogThreads) * kLinesPerThread);
  const auto tail = log.Since(0);
  CHECK(!tail.empty() && tail.back().seq == log.LastSeq());

  if (sovereign::test::Failures() != 0) {
    return 1;
  }
  std::cout << "OK: " << kRounds * 4 << " requests, " << kRounds << " resumes, " << kLogThreads * kLinesPerThread
            << " log lines\n";
  return 0;
}
