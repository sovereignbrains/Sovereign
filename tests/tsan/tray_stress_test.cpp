// ThreadSanitizer stress test (tests/tsan): the tray's portable logic -
// TrayModel, the log page parser, the app rules - with threads racing each
// other. In the tray the model lives on the worker thread alone and what the
// window shows is a copy published under a lock; the parsers run on the
// worker and on the checks' thread. So two things are raced here:
//   * several threads each driving their own model and parsers at once - any
//     hidden shared state (a static cache, a global) is a TSan report, and the
//     results must match what one thread alone gets;
//   * a worker publishing its model's view under a mutex while a reader (the
//     UI thread) copies it - the tray's own pattern.
// Results are compared on the main thread: check.h's counter isn't atomic.

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app_rules.h"
#include "check.h"
#include "log_lines.h"
#include "tray_model.h"

namespace {

using namespace sovereign::tray;

constexpr int kThreads = 4;
constexpr int kRounds = 3000;
constexpr std::int64_t kPublishes = std::int64_t{4} * kRounds;

constexpr std::string_view kConfig = R"({
  "outbounds": [{"type":"selector","tag":"proxy","outbounds":["a"]},{"type":"anytls","tag":"a"},{"type":"direct","tag":"direct"}],
  "route": {"rules": [{"action":"sniff"},{"protocol":"dns","action":"hijack-dns"},
                      {"ip_is_private":true,"outbound":"direct"}], "final": "proxy"}
})";

constexpr std::string_view kLogs =
    R"({"cmd":"box_logs","entries":[)"
    R"({"seq":7,"level":"error","message":"ERROR[0001] outbound/naive[Naive]: dial failed","time":1790000000123},)"
    R"({"seq":8,"level":"info","message":"started"}],"next":8,"more":true})";

Stats Running(std::int64_t up, std::int64_t down, std::int64_t connections, std::string sha) {
  Stats stats;
  stats.running = true;
  stats.uplinkBytes = up;
  stats.downlinkBytes = down;
  stats.connections = connections;
  stats.generation = 1;
  stats.configSha256 = std::move(sha);
  return stats;
}

// One round of a model's life: the service answers, the box starts and runs,
// the config changes under it (a restart), the user turns it off. Returns
// what it saw as one string, so rounds and threads can be compared.
std::string DriveModel(TrayModel::Clock::time_point start) {
  std::string seen;
  TrayModel model(true);
  auto now = start;
  const auto step = [&](Action action) { seen += static_cast<char>('0' + static_cast<int>(action)); };
  model.SetExpectedConfig("aaaa");
  step(model.OnPoll(Stats{}, now));  // up, not running: start
  model.OnStartResult("", now);
  for (std::int64_t i = 1; i <= 3; ++i) {
    now += std::chrono::seconds(1);
    step(model.OnPoll(Running(1000 * i, 4000 * i, i, "aaaa"), now));
  }
  seen += static_cast<char>('0' + static_cast<int>(model.GetDisplay()));
  seen += std::to_string(static_cast<int>(model.DownRate()));
  model.SetExpectedConfig("bbbb");  // a subscription refresh: restart once
  now += std::chrono::seconds(1);
  step(model.OnPoll(Running(0, 0, 0, "aaaa"), now));
  model.OnStopResult("");
  step(model.SetWantOn(false, now));
  seen += static_cast<char>('0' + static_cast<int>(model.GetDisplay()));
  return seen;
}

std::string Parse() {
  std::string out;
  if (const auto page = ParseLogsResponse(kLogs)) {
    for (const LogLine& line : page->lines) {
      out += line.level + "|" + line.message + "|" + std::to_string(line.timeMs) + ";";
    }
  }
  out += ApplyAppRules(kConfig, AppsMode::Exclude, {"steam.exe", "qbittorrent.exe"});
  out += ApplyAppRules(kConfig, AppsMode::Include, {"chrome.exe"});
  return out;
}

void TestIndependentThreads() {
  const auto start = TrayModel::Clock::now();
  const std::string model = DriveModel(start);
  const std::string parsed = Parse();
  std::vector<int> mismatches(kThreads, 0);
  {
    std::vector<std::jthread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < kRounds; ++i) {
          mismatches[static_cast<std::size_t>(t)] += (DriveModel(start) != model) + (Parse() != parsed);
        }
      });
    }
  }
  for (const int m : mismatches) {
    CHECK(m == 0);
  }
}

// What the worker publishes for the window (main.cpp keeps such a copy in its
// shared state).
struct View {
  Display display = Display::ServiceDown;
  double down = 0;
  std::int64_t connections = 0;
  std::string error;
};

void TestPublishedView() {
  std::mutex mutex;
  View shared;
  std::atomic<bool> done = false;
  std::int64_t lastSeen = 0;
  bool wentBack = false;
  std::jthread reader([&] {
    for (;;) {
      const bool finished = done.load();  // set after the last publish: one more read sees it
      View copy;
      {
        const std::scoped_lock lock(mutex);
        copy = shared;
      }
      wentBack = wentBack || copy.connections < lastSeen;  // published in order: never backwards
      lastSeen = copy.connections;
      if (finished) {
        break;
      }
    }
  });
  {
    TrayModel model(true);
    auto now = TrayModel::Clock::now();
    model.OnPoll(Stats{}, now);
    model.OnStartResult("", now);
    for (std::int64_t i = 1; i <= kPublishes; ++i) {
      now += std::chrono::seconds(1);
      model.OnPoll(Running(i, 2 * i, i, ""), now);
      const std::scoped_lock lock(mutex);
      shared = View{.display = model.GetDisplay(), .down = model.DownRate(), .connections = model.Connections(),
                    .error = model.LastError()};
    }
  }
  done = true;
  reader.join();
  CHECK(!wentBack);
  CHECK(lastSeen == kPublishes);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestIndependentThreads();
    TestPublishedView();
  } catch (const std::exception& e) {
    std::cerr << "exception: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "unknown exception\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
