// Reading the service's box_delays answers (src/tray/delays.h).

#include <exception>
#include <iostream>

#include "check.h"
#include "delays.h"

namespace {

using namespace sovereign::tray;

void TestParsesResults() {
  const auto delays = ParseDelaysResponse(
      R"({"cmd":"box_delays","results":[{"tag":"nl","delay":48,"jitter":3,"loss":250,"samples":9,"connect":412},)"
      R"({"tag":"fi","error":"i/o timeout"},)"
      R"({"tag":"de","pending":true},{"delay":5},"junk",{"tag":"big","delay":1e9},{"tag":7,"delay":1}]})");
  CHECK(delays.has_value());
  if (!delays) {
    return;
  }
  CHECK(delays->size() == 3);
  CHECK(delays->contains("nl") && delays->at("nl").state == Delay::State::Ok && delays->at("nl").ms == 48);
  CHECK(delays->contains("nl") && delays->at("nl").jitter == 3 && delays->at("nl").loss == 100 &&
        delays->at("nl").samples == 9 && delays->at("nl").connect == 412);
  CHECK(delays->contains("fi") && delays->at("fi").state == Delay::State::Failed &&
        delays->at("fi").error == "i/o timeout");
  CHECK(delays->contains("de") && delays->at("de").state == Delay::State::Pending);
  CHECK(!delays->contains("big"));  // not an integer: skipped, not thrown
}

void TestRejectsWhatIsNotAnAnswer() {
  CHECK(!ParseDelaysResponse("not json"));
  CHECK(!ParseDelaysResponse(R"({"cmd":"error","message":"box not running"})"));
  CHECK(!ParseDelaysResponse(R"({"cmd":"box_delays"})"));
  CHECK(!ParseDelaysResponse(R"({"cmd":"box_delays","results":{}})"));
  CHECK(ParseDelaysResponse(R"({"cmd":"box_delays","results":[]})").has_value());
}

Delay Ok(int ms, int jitter = 0, int loss = 0) {
  return {.state = Delay::State::Ok, .ms = ms, .jitter = jitter, .loss = loss, .samples = 9};
}

// Auto picks the best at once when it has nothing (or a dead server), and
// moves off a live one only for a clear win, twice in a row.
void TestJudgeAuto() {
  const std::vector<std::string> members = {"nl", "fi", "de"};
  std::map<std::string, Delay> delays = {{"nl", Ok(80)}, {"fi", Ok(60, 15)}, {"de", Ok(70)}};
  // The spread counts: fi's 60 ±15 is worse than de's steady 70.
  AutoPick pick = JudgeAuto({}, members, delays);
  CHECK(pick.server == "de" && pick.candidate.empty());

  // A few milliseconds better is noise: no switch, no candidate.
  delays = {{"nl", Ok(64)}, {"fi", Ok(90)}, {"de", Ok(70)}};
  pick = JudgeAuto(pick, members, delays);
  CHECK(pick.server == "de" && pick.candidate.empty());

  // A clear win once makes a candidate; twice, the pick.
  delays = {{"nl", Ok(40)}, {"fi", Ok(90)}, {"de", Ok(70)}};
  pick = JudgeAuto(pick, members, delays);
  CHECK(pick.server == "de" && pick.candidate == "nl" && pick.wins == 1);
  pick = JudgeAuto(pick, members, delays);
  CHECK(pick.server == "nl" && pick.candidate.empty());

  // A win that doesn't repeat is forgotten.
  delays = {{"nl", Ok(80)}, {"fi", Ok(40)}, {"de", Ok(90)}};
  pick = JudgeAuto(pick, members, delays);
  CHECK(pick.server == "nl" && pick.candidate == "fi" && pick.wins == 1);
  delays = {{"nl", Ok(80)}, {"fi", Ok(78)}, {"de", Ok(90)}};
  pick = JudgeAuto(pick, members, delays);
  CHECK(pick.server == "nl" && pick.candidate.empty());

  // Losses weigh: 5% lost is worse than 40 ms more.
  delays = {{"nl", Ok(50, 0, 5)}, {"fi", Ok(90)}, {"de", Ok(95)}};
  CHECK(JudgeAuto({}, members, delays).server == "fi");

  // The one in use failed: the best at once.
  delays = {{"nl", {.state = Delay::State::Failed, .error = "timeout"}}, {"fi", Ok(90)}, {"de", Ok(95)}};
  CHECK(JudgeAuto({.server = "nl"}, members, delays).server == "fi");
  // Not measured this time: kept.
  delays = {{"nl", Delay{}}, {"fi", Ok(20)}};
  CHECK(JudgeAuto({.server = "nl"}, members, delays).server == "nl");
  // Gone from the group: replaced.
  CHECK(JudgeAuto({.server = "se"}, members, delays).server == "fi");
  // Nothing answered: kept as it is.
  delays = {{"fi", {.state = Delay::State::Failed}}};
  CHECK(JudgeAuto({.server = "nl", .candidate = "de", .wins = 1}, members, delays).candidate == "de");
}

// WARP's next server: untried, answered first and best first, failed last.
void TestNextWarpServer() {
  const std::vector<std::string> servers = {"nl", "fi", "de", "se"};
  std::map<std::string, Delay> delays = {
      {"nl", Ok(80)}, {"fi", {.state = Delay::State::Failed}}, {"de", Ok(40)}, {"se", Delay{}}};
  CHECK(NextWarpServer(servers, {"nl"}, delays) == "de");
  CHECK(NextWarpServer(servers, {"nl", "de"}, delays) == "se");
  CHECK(NextWarpServer(servers, {"nl", "de", "se"}, delays) == "fi");
  CHECK(NextWarpServer(servers, servers, delays).empty());
  CHECK(NextWarpServer(servers, {}, {}) == "nl");  // nothing measured: in their order
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestParsesResults();
    TestRejectsWhatIsNotAnAnswer();
    TestJudgeAuto();
    TestNextWarpServer();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
