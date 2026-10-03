// The subscription relay's protocol (src/tray/relay.h): what the tray sends
// to the worker (tools/relay/worker.js) and how it reads a big answer's plan.

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "relay.h"
#include "subscription.h"

namespace {

using namespace sovereign::tray;
using nlohmann::json;

constexpr const char* kJob = "0f9e8d7c-6b5a-4f3e-9d2c-1b0a9f8e7d6c";

void TestUrlAndKey() {
  CHECK(IsRelayUrl("https://fetch.qz7mtk4rwp.workers.dev/"));
  CHECK(IsRelayUrl("https://relay.example.com"));
  CHECK(!IsRelayUrl("http://fetch.example.workers.dev/"));     // https only: the key goes in a header
  CHECK(!IsRelayUrl("https://"));
  CHECK(!IsRelayUrl("https://x.workers.dev/?key=1"));          // no query: the key isn't put in URLs
  CHECK(!IsRelayUrl("https://user@x.workers.dev/"));
  CHECK(!IsRelayUrl("https://x.workers.dev/ path"));
  CHECK(!IsRelayUrl("https://\xD0\xBF\xD1\x80.workers.dev/"));  // non-ASCII: punycode it

  CHECK(IsRelayKey("abcdefghijkmnpqrstuvwxyz23456789abcdefgh"));
  CHECK(!IsRelayKey("short"));
  CHECK(!IsRelayKey("with space inside the key 1234"));
  CHECK(!IsRelayKey(std::string(257, 'a')));

  CHECK(RelayBase("https://x.workers.dev") == "https://x.workers.dev/");
  CHECK(RelayBase("https://x.workers.dev//") == "https://x.workers.dev/");
}

void TestRequestBody() {
  const json body = json::parse(
      RelayRequestBody("https://panel.example/sub/abc", "sing-box 1.14.2", "0123456789abcdef0123456789abcdef"));
  CHECK(body["url"] == "https://panel.example/sub/abc");
  CHECK(body["headers"]["User-Agent"] == "sing-box 1.14.2");
  CHECK(body["headers"]["x-hwid"] == "0123456789abcdef0123456789abcdef");
  CHECK(body["headers"].size() == 2);  // nothing else of the machine's
  // Not a hwid (empty, or something odd): not sent.
  CHECK(!json::parse(RelayRequestBody("https://p/s", "ua", ""))["headers"].contains("x-hwid"));
  CHECK(!json::parse(RelayRequestBody("https://p/s", "ua", "not a hwid"))["headers"].contains("x-hwid"));
}

void TestPlan() {
  const auto plan = ParseRelayPlan(kJob, "164120", "16000", kMaxSubscriptionBytes);
  CHECK(plan.has_value());
  if (plan) {
    CHECK(plan->count == 11);  // 10 full pieces and one of 4120
    CHECK(RelayChunkPath(*plan, 0) == std::string("chunk?job=") + kJob + "&n=0");
    CHECK(RelayChunkPath(*plan, 10) == std::string("chunk?job=") + kJob + "&n=10");
  }
  const auto exact = ParseRelayPlan(kJob, "32000", "16000", kMaxSubscriptionBytes);
  CHECK(exact && exact->count == 2);

  CHECK(!ParseRelayPlan("not-a-uuid", "100", "10", kMaxSubscriptionBytes));
  CHECK(!ParseRelayPlan("0F9E8D7C-6B5A-4F3E-9D2C-1B0A9F8E7D6C", "100", "10", kMaxSubscriptionBytes));  // its own case
  CHECK(!ParseRelayPlan(kJob, "", "16000", kMaxSubscriptionBytes));
  CHECK(!ParseRelayPlan(kJob, "12x", "16000", kMaxSubscriptionBytes));
  CHECK(!ParseRelayPlan(kJob, "-5", "16000", kMaxSubscriptionBytes));
  CHECK(!ParseRelayPlan(kJob, "100", "0", kMaxSubscriptionBytes));
  CHECK(!ParseRelayPlan(kJob, "0", "16000", kMaxSubscriptionBytes));
  CHECK(!ParseRelayPlan(kJob, std::to_string(kMaxSubscriptionBytes + 1), "16000", kMaxSubscriptionBytes));
  CHECK(!ParseRelayPlan(kJob, "100000", "1", kMaxSubscriptionBytes));  // 100000 pieces: no
  CHECK(!ParseRelayPlan(kJob, "99999999999999999999999", "16000", kMaxSubscriptionBytes));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestUrlAndKey();
    TestRequestBody();
    TestPlan();
  } catch (const std::exception& e) {
    std::cerr << "exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
