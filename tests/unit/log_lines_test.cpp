// Reading the service's box_logs answers (src/tray/log_lines.h).

#include <exception>
#include <iostream>

#include "check.h"
#include "log_lines.h"

namespace {

using namespace sovereign::tray;

void TestParsesAPage() {
  const auto page = ParseLogsResponse(
      R"({"cmd":"box_logs","entries":[)"
      R"({"seq":7,"level":"error","message":"ERROR[0001] outbound/naive[Naive]: dial failed","time":1790000000123},)"
      R"({"seq":8,"level":"info","message":"started"}],"next":8,"more":true})");
  CHECK(page.has_value());
  if (!page) {
    return;
  }
  CHECK(page->next == 8 && page->more);
  CHECK(page->lines.size() == 2);
  CHECK(page->lines[0].level == "error" && page->lines[0].timeMs == 1790000000123);
  CHECK(page->lines[0].message == "outbound/naive[Naive]: dial failed");
  CHECK(page->lines[1].timeMs == 0 && page->lines[1].message == "started");  // an older service: no time
}

void TestRejectsWhatIsNotAPage() {
  CHECK(!ParseLogsResponse("not json"));
  CHECK(!ParseLogsResponse(R"({"cmd":"error","message":"gocore not loaded"})"));
  CHECK(!ParseLogsResponse(R"({"cmd":"box_logs","entries":[]})"));  // no next
  CHECK(!ParseLogsResponse(R"({"cmd":"box_logs","entries":{},"next":1})"));
}

// Fields of the wrong type (found by tests/fuzz): not a page, or defaults - never an exception.
void TestHostileTypes() {
  CHECK(!ParseLogsResponse(R"({"cmd":1})"));
  const auto page = ParseLogsResponse(R"({"cmd":"box_logs","entries":[{"message":5,"time":"x","level":[]}],"next":1,"more":"yes"})");
  CHECK(page.has_value());
  if (page) {
    CHECK(page->lines.size() == 1 && page->lines[0].message.empty() && page->lines[0].timeMs == 0 && !page->more);
  }
}

void TestStripsOnlyTheLevelPrefix() {
  CHECK(StripCoreLevelPrefix("INFO[0012] [3620113478 0ms] inbound/tun[tun-in]: x") ==
        "[3620113478 0ms] inbound/tun[tun-in]: x");
  CHECK(StripCoreLevelPrefix("WARN[0000]") == "");
  CHECK(StripCoreLevelPrefix("plain message") == "plain message");
  CHECK(StripCoreLevelPrefix("INFO[abc] x") == "INFO[abc] x");
  CHECK(StripCoreLevelPrefix("[0012] x") == "[0012] x");
  CHECK(StripCoreLevelPrefix("") == "");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestParsesAPage();
    TestRejectsWhatIsNotAPage();
    TestStripsOnlyTheLevelPrefix();
    TestHostileTypes();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
