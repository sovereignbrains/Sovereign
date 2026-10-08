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

void TestUrlHost() {
  CHECK(UrlHost(L"https://packetlab.tech/sub/0123abcd") == L"packetlab.tech");
  CHECK(UrlHost(L"https://sub.example.com:8443?token=secret") == L"sub.example.com:8443");
  CHECK(UrlHost(L"https://user:pass@host.example#frag") == L"host.example");  // no credentials on screen
  CHECK(UrlHost(L"https://host.example") == L"host.example");
  CHECK(UrlHost(L"no scheme").empty());
  CHECK(UrlHost(L"").empty());
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

// What panels say about a subscription (Marzban, Remnawave, 3x-ui write it
// this way): traffic in bytes, the end of the paid period, support's link.
void TestPanelHeaders() {
  const auto usage = ParseSubscriptionUserinfo("upload=1048576; download=2097152; total=107374182400; expire=1798761600");
  CHECK(usage && usage->upload == 1048576 && usage->download == 2097152);
  CHECK(usage && usage->total == 107374182400ULL && usage->expire == 1798761600);
  const auto unlimited = ParseSubscriptionUserinfo("upload=0;download=5;total=0;expire=0");  // no spaces, no limit
  CHECK(unlimited && unlimited->download == 5 && unlimited->total == 0 && unlimited->expire == 0);
  const auto floaty = ParseSubscriptionUserinfo("upload=12.5; download=1e3; total=10737418240");
  CHECK(floaty && floaty->upload == 12 && floaty->total == 10737418240ULL);  // the integer part
  CHECK(!ParseSubscriptionUserinfo(""));
  CHECK(!ParseSubscriptionUserinfo("hello; world=x"));

  CHECK(ParseLinkHeader(" https://t.me/support_bot ") == std::optional<std::string>("https://t.me/support_bot"));
  CHECK(ParseLinkHeader("tg://resolve?domain=support_bot").has_value());
  CHECK(ParseLinkHeader("http://panel.example.com/sub/abc").has_value());
  CHECK(!ParseLinkHeader("javascript:alert(1)"));
  CHECK(!ParseLinkHeader("file:///C:/Windows/System32/calc.exe"));
  CHECK(!ParseLinkHeader("https://t.me/a b"));      // a space: not one link
  CHECK(!ParseLinkHeader("https://t.me/\x01"));     // a control character
  CHECK(!ParseLinkHeader("https://t.me/\xD0\xB0"));  // not ASCII
  CHECK(!ParseLinkHeader("https://" + std::string(kMaxLinkHeader, 'a')));
  CHECK(!ParseLinkHeader(""));
}

// A panel's stub dressed as a server is told apart from a subscription.
void TestProviderNotice() {
  CHECK(ProviderNotice(R"({"outbounds":[{"type":"vless","tag":"🔴 Приложение не поддерживает HWID"},)"
                       R"({"type":"direct","tag":"direct"}]})") == "🔴 Приложение не поддерживает HWID");
  CHECK(ProviderNotice(R"({"outbounds":[{"type":"vless","tag":"Превышен лимит устройств"}]})") ==
        "Превышен лимит устройств");
  CHECK(ProviderNotice(R"({"outbounds":[{"type":"vless","tag":"🇩🇪 Germany"}]})").empty());  // one real server
  CHECK(ProviderNotice(R"({"outbounds":[{"type":"vless","tag":"🔴 a"},{"type":"trojan","tag":"b"}]})").empty());
  CHECK(ProviderNotice("not json").empty());
}

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestPanelHeaders();
    TestHttpsOnly();
    TestUrlHost();
    TestConfigCheck();
    TestUpdateInterval();
    TestTunDetection();
    TestProviderNotice();
    CHECK(!sovereign::tray::ConfigHasTun(R"({"inbounds":[{"type":7}]})"));  // found by tests/fuzz: no exception
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
