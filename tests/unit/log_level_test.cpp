// The log level the tray puts over the config's (src/tray/log_level.h).

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>

#include "check.h"
#include "log_level.h"

namespace {

using nlohmann::json;
using sovereign::tray::ApplyLogLevel;
using sovereign::tray::IsLogLevel;

void TestLevels() {
  CHECK(IsLogLevel("debug") && IsLogLevel("info") && IsLogLevel("warn") && IsLogLevel("error"));
  CHECK(!IsLogLevel("") && !IsLogLevel("WARN") && !IsLogLevel("trace"));
}

void TestApplies() {
  const json added = json::parse(ApplyLogLevel(R"({"outbounds":[]})", "debug"));
  CHECK(added["log"]["level"] == "debug" && added["log"]["disabled"] == false);
  CHECK(added["outbounds"].is_array());
  // The config's other log settings stay; its own "disabled" doesn't.
  const json over = json::parse(ApplyLogLevel(R"({"log":{"level":"warn","timestamp":true,"disabled":true}})", "info"));
  CHECK(over["log"]["level"] == "info" && over["log"]["timestamp"] == true && over["log"]["disabled"] == false);
}

void TestLeavesAlone() {
  CHECK(json::parse(ApplyLogLevel(R"({"log":{"level":"warn"}})", "")) == json::parse(R"({"log":{"level":"warn"}})"));
  CHECK(json::parse(ApplyLogLevel(R"({"log":{"level":"warn"}})", "loud")) == json::parse(R"({"log":{"level":"warn"}})"));
  CHECK(ApplyLogLevel("not json", "debug") == "not json");
  CHECK(ApplyLogLevel("[1]", "debug") == "[1]");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestLevels();
    TestApplies();
    TestLeavesAlone();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
