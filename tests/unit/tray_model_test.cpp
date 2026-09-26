// The tray's decisions (src/tray/tray_model.h) without Win32: when to send
// box_start/box_stop, what the icon shows, how rates come out of box_stats.

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>

#include "check.h"
#include "tray_model.h"

namespace {

using sovereign::tray::Action;
using sovereign::tray::Display;
using sovereign::tray::Stats;
using sovereign::tray::TrayModel;
using namespace std::chrono_literals;

const TrayModel::Clock::time_point t0{};

Stats Running(std::int64_t down, std::int64_t up, std::int64_t generation = 1) {
  return Stats{.running = true, .uplinkBytes = up, .downlinkBytes = down, .connections = 2, .generation = generation};
}

const Stats kStopped{};

void TestStartsWhenWantedAndServiceUp() {
  TrayModel m(true);
  CHECK(m.OnPoll(std::nullopt, t0) == Action::None);  // no service yet
  CHECK(m.GetDisplay() == Display::ServiceDown);

  CHECK(m.OnPoll(kStopped, t0 + 1s) == Action::Start);
  CHECK(m.GetDisplay() == Display::Starting);
  CHECK(m.OnPoll(kStopped, t0 + 2s) == Action::None);  // start still in flight: no second one

  m.OnStartResult("", t0 + 2s);
  CHECK(m.GetDisplay() == Display::On);
  CHECK(m.OnPoll(Running(0, 0), t0 + 3s) == Action::None);
  CHECK(m.GetDisplay() == Display::On);
}

void TestRates() {
  TrayModel m(true);
  m.OnPoll(Running(1000, 100), t0);
  CHECK(m.DownRate() == 0 && m.UpRate() == 0);  // one sample only
  m.OnPoll(Running(3000, 600), t0 + 2s);
  CHECK(m.DownRate() == 1000 && m.UpRate() == 250);
  CHECK(m.Connections() == 2);
  // A new generation restarted the counters: no rate across the reset.
  m.OnPoll(Running(10, 10, 2), t0 + 3s);
  CHECK(m.DownRate() == 0 && m.UpRate() == 0);
}

void TestFailedStartRetriesAfterBackoff() {
  TrayModel m(true);
  CHECK(m.OnPoll(kStopped, t0) == Action::Start);
  m.OnStartResult("parse config: bad", t0);
  CHECK(m.GetDisplay() == Display::Error);
  CHECK(m.LastError() == "parse config: bad");
  CHECK(m.OnPoll(kStopped, t0 + 5s) == Action::None);
  CHECK(m.OnPoll(kStopped, t0 + TrayModel::kRetryAfter) == Action::Start);
  m.OnStartResult("", t0 + TrayModel::kRetryAfter);
  CHECK(m.LastError().empty());
  CHECK(m.GetDisplay() == Display::On);
}

void TestServiceRestartRestartsTheBox() {
  TrayModel m(true);
  m.OnPoll(kStopped, t0);
  m.OnStartResult("", t0);
  m.OnPoll(Running(0, 0), t0 + 1s);
  CHECK(m.OnPoll(std::nullopt, t0 + 2s) == Action::None);  // service restarting
  CHECK(m.GetDisplay() == Display::ServiceDown);
  CHECK(m.OnPoll(kStopped, t0 + 3s) == Action::Start);      // back, box gone: start again
}

void TestOffStopsOnceAndStaysOff() {
  TrayModel m(true);
  m.OnPoll(kStopped, t0);
  m.OnStartResult("", t0);
  m.OnPoll(Running(0, 0), t0 + 1s);

  CHECK(m.SetWantOn(false, t0 + 2s) == Action::Stop);
  m.OnStopResult("");
  CHECK(m.GetDisplay() == Display::Off);
  CHECK(m.OnPoll(kStopped, t0 + 3s) == Action::None);
  CHECK(m.GetDisplay() == Display::Off);

  CHECK(m.SetWantOn(true, t0 + 4s) == Action::Start);  // back on: immediately
}

void TestOffWhileServiceDownDoesNothing() {
  TrayModel m(true);
  m.OnPoll(std::nullopt, t0);
  CHECK(m.SetWantOn(false, t0) == Action::None);
  CHECK(m.OnPoll(kStopped, t0 + 1s) == Action::None);
  CHECK(m.GetDisplay() == Display::Off);
}

void TestAlreadyStartedElsewhereCountsAsOn() {
  TrayModel m(true);
  m.OnPoll(kStopped, t0);
  m.OnStartResult("box already started; call box_stop first", t0);
  CHECK(m.GetDisplay() == Display::Error);
  CHECK(m.OnPoll(Running(0, 0), t0 + 1s) == Action::None);
  CHECK(m.GetDisplay() == Display::On);
  CHECK(m.LastError().empty());
}

void TestStartsOffAndStaysOff() {
  TrayModel m(false);
  CHECK(m.OnPoll(kStopped, t0) == Action::None);
  CHECK(m.GetDisplay() == Display::Off);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestStartsWhenWantedAndServiceUp();
    TestRates();
    TestFailedStartRetriesAfterBackoff();
    TestServiceRestartRestartsTheBox();
    TestOffStopsOnceAndStaysOff();
    TestOffWhileServiceDownDoesNothing();
    TestAlreadyStartedElsewhereCountsAsOn();
    TestStartsOffAndStaysOff();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
