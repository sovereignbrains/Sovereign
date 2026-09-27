// Reading the service's box_delays answers (src/tray/delays.h).

#include <exception>
#include <iostream>

#include "check.h"
#include "delays.h"

namespace {

using namespace sovereign::tray;

void TestParsesResults() {
  const auto delays = ParseDelaysResponse(
      R"({"cmd":"box_delays","results":[{"tag":"nl","delay":48},{"tag":"fi","error":"i/o timeout"},)"
      R"({"tag":"de","pending":true},{"delay":5},"junk",{"tag":"big","delay":1e9},{"tag":7,"delay":1}]})");
  CHECK(delays.has_value());
  if (!delays) {
    return;
  }
  CHECK(delays->size() == 3);
  CHECK(delays->contains("nl") && delays->at("nl").state == Delay::State::Ok && delays->at("nl").ms == 48);
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

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestParsesResults();
    TestRejectsWhatIsNotAnAnswer();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
