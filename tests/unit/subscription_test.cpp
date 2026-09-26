// The subscription's pure rules (src/tray/subscription.h).

#include <chrono>
#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "subscription.h"

namespace {

using namespace sovereign::tray;
using namespace std::chrono_literals;

void TestHttpsOnly() {
  CHECK(IsHttpsUrl(L"https://packetlab.tech/sub/0123abcd"));
  CHECK(IsHttpsUrl(L"HTTPS://Example.com"));
  CHECK(!IsHttpsUrl(L"http://packetlab.tech/sub/0123abcd"));  // the token would travel in clear
  CHECK(!IsHttpsUrl(L"https://"));
  CHECK(!IsHttpsUrl(L"https:///path"));
  CHECK(!IsHttpsUrl(L"https://host/a b"));
  CHECK(!IsHttpsUrl(L"https://host/a\r\nX-Injected: 1"));
  CHECK(!IsHttpsUrl(L"vless://uuid@host:443"));
  CHECK(!IsHttpsUrl(L""));
}

void TestConfigCheck() {
  const auto ok = CheckSubscriptionConfig(R"({"outbounds":[{"type":"direct"},{"type":"anytls"}]})");
  CHECK(ok.ok && ok.outbounds == 2 && ok.error.empty());
  CHECK(!CheckSubscriptionConfig("<html>login</html>").ok);
  CHECK(!CheckSubscriptionConfig("[1,2]").ok);
  CHECK(!CheckSubscriptionConfig(R"({"inbounds":[]})").ok);
  CHECK(!CheckSubscriptionConfig(R"({"outbounds":[]})").ok);
  CHECK(!CheckSubscriptionConfig(R"({"outbounds":{}})").ok);
  CHECK(!CheckSubscriptionConfig(std::string(kMaxSubscriptionBytes + 1, ' ')).ok);
  CHECK(!CheckSubscriptionConfig("").error.empty());
}

void TestUpdateInterval() {
  CHECK(ParseUpdateInterval("12") == 12h);
  CHECK(ParseUpdateInterval(" 24 ") == 24h);
  CHECK(ParseUpdateInterval("100000") == std::chrono::hours(24 * 7));
  CHECK(!ParseUpdateInterval("").has_value());
  CHECK(!ParseUpdateInterval("0").has_value());
  CHECK(!ParseUpdateInterval("-3").has_value());
  CHECK(!ParseUpdateInterval("12h").has_value());
  CHECK(!ParseUpdateInterval("soon").has_value());
}

void TestTunDetection() {
  CHECK(ConfigHasTun(R"({"inbounds":[{"type":"tun","address":["172.19.0.1/30"]},{"type":"mixed"}]})"));
  CHECK(!ConfigHasTun(R"({"inbounds":[{"type":"mixed","listen_port":2080}]})"));
  CHECK(!ConfigHasTun(R"({"outbounds":[]})"));
  CHECK(!ConfigHasTun("not json"));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestHttpsOnly();
    TestConfigCheck();
    TestUpdateInterval();
    TestTunDetection();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
