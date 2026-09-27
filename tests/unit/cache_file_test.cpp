// The tray's cache_file setting (src/tray/cache_file.h).

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "cache_file.h"
#include "check.h"

namespace {

using nlohmann::json;
using sovereign::tray::ApplyCacheFile;

constexpr const char* kPath = R"(C:\ProgramData\Sovereign\cache.db)";

void TestAddsWhenAbsent() {
  const json c = json::parse(ApplyCacheFile(R"({"outbounds":[{"type":"direct","tag":"d"}]})", std::string(kPath)));
  CHECK(c["experimental"]["cache_file"]["enabled"] == true);
  CHECK(c["experimental"]["cache_file"]["path"] == kPath);
  CHECK(c["outbounds"][0]["tag"] == "d");  // nothing else touched
}

void TestKeepsTheSubscriptionsOtherSettings() {
  // A subscription's own relative path would land in System32 under the service.
  const json c = json::parse(ApplyCacheFile(
      R"({"experimental":{"cache_file":{"enabled":false,"path":"cache.db","store_rdrc":true},"clash_api":{"x":1}}})",
      std::string(kPath)));
  CHECK(c["experimental"]["cache_file"]["enabled"] == true);
  CHECK(c["experimental"]["cache_file"]["path"] == kPath);
  CHECK(c["experimental"]["cache_file"]["store_rdrc"] == true);
  CHECK(c["experimental"]["clash_api"]["x"] == 1);
}

void TestNotAnObject() {
  CHECK(ApplyCacheFile("not json", std::string(kPath)) == "not json");
  CHECK(ApplyCacheFile("[1,2]", std::string(kPath)) == "[1,2]");
  // A non-object experimental/cache_file is replaced, not kept broken.
  const json c = json::parse(ApplyCacheFile(R"({"experimental":5})", std::string(kPath)));
  CHECK(c["experimental"]["cache_file"]["enabled"] == true);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestAddsWhenAbsent();
    TestKeepsTheSubscriptionsOtherSettings();
    TestNotAnObject();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
