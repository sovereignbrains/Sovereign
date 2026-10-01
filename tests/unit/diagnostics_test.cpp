// The checks' pure parts (src/tray/diagnostics.h): what's read off the wire
// and the log, and how results are judged.

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "check.h"
#include "diagnostics.h"

namespace {

using namespace sovereign::tray;

std::string Bytes(std::initializer_list<int> values) {
  std::string out;
  for (const int v : values) {
    out.push_back(static_cast<char>(v));
  }
  return out;
}

void TestTraceAndIp() {
  const auto trace = ParseTrace("fl=1\nh=www.cloudflare.com\nip=5.83.147.210\nts=1\nloc=DE\ncolo=FRA\n");
  CHECK(trace && trace->ip == "5.83.147.210" && trace->loc == "DE");
  const auto lower = ParseTrace("ip=2a0c:4ac1:4:3e::a\r\nloc=de\r\n");
  CHECK(lower && lower->loc.empty());  // not two capitals: dropped
  CHECK(!ParseTrace("<html>blocked</html>"));
  CHECK(ParseQuotedIp(" \"46.148.140.142\"\n") == std::optional<std::string>("46.148.140.142"));
  CHECK(!ParseQuotedIp("\"999.1.1.1\""));
  CHECK(LooksLikeIp("1.2.3.4") && LooksLikeIp("::1") && !LooksLikeIp("1.2.3") && !LooksLikeIp("a.b.c.d"));
}

void TestStun() {
  StunId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::uint8_t>(i + 1);
  }
  const std::string request = StunRequest(id);
  CHECK(request.size() == 20 && request[1] == 0x01 && request[4] == 0x21 && request[19] == 12);

  // 5.83.147.210:50000 as XOR-MAPPED-ADDRESS: port ^ 0x2112, address ^ 0x2112A442.
  std::string response = Bytes({0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42});
  response += request.substr(8, 12);
  response += Bytes({0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xE1, 0xD2, 5 ^ 0x21, 83 ^ 0x12, 147 ^ 0xA4, 210 ^ 0x42});
  CHECK(ParseStunResponse(response, id) == std::optional<std::string>("5.83.147.210"));
  StunId other = id;
  other[0] = 99;
  CHECK(!ParseStunResponse(response, other));  // someone else's answer
  CHECK(!ParseStunResponse(response.substr(0, 25), id));

  // Plain MAPPED-ADDRESS (an old server).
  std::string plain = Bytes({0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42});
  plain += request.substr(8, 12);
  plain += Bytes({0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x13, 0x88, 10, 0, 0, 7});
  CHECK(ParseStunResponse(plain, id) == std::optional<std::string>("10.0.0.7"));

  // IPv6, XOR'd with the cookie and the id: 2001:db8::1.
  std::string v6 = Bytes({0x01, 0x01, 0x00, 0x18, 0x21, 0x12, 0xA4, 0x42});
  v6 += request.substr(8, 12);
  std::string address = Bytes({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  const std::string mask = Bytes({0x21, 0x12, 0xA4, 0x42}) + request.substr(8, 12);
  for (std::size_t i = 0; i < 16; ++i) {
    address[i] = static_cast<char>(address[i] ^ mask[i]);
  }
  v6 += Bytes({0x00, 0x20, 0x00, 0x14, 0x00, 0x02, 0x00, 0x00}) + address;
  CHECK(ParseStunResponse(v6, id) == std::optional<std::string>("2001:db8:0:0:0:0:0:1"));
}

void TestDnsLeak() {
  const auto seen = ParseDnsLeak(
      R"([{"ip":"5.83.147.210","country":"de","org":"PFWeb","type":"ip"},)"
      R"({"ip":"188.122.68.217","country":"de","country_name":"Germany","org":"i3D.net B.V","type":"dns"},)"
      R"({"ip":"2a00:1637:409:93:9999::217","country":"de","asn":"AS49544 i3D.net B.V","type":"dns"},)"
      R"({"ip":"x","type":"dns"},{"type":"conclusion"}])");
  CHECK(seen.size() == 2);
  const auto ok = JudgeDnsLeak(seen);
  CHECK(ok.status == CheckResult::Status::Ok && ok.summary == "i3D.net B.V (DE), AS49544 i3D.net B.V (DE)");
  const auto leak = JudgeDnsLeak({{.ip = "1.1.1.1", .country = "de", .org = "Cloudflare"},
                                  {.ip = "95.167.0.1", .country = "ru", .org = "Rostelecom"}});
  CHECK(leak.status == CheckResult::Status::Fail);
  CHECK(JudgeDnsLeak({}).status == CheckResult::Status::Warn);
  CHECK(ParseDnsLeak("not json").empty());
}

void TestLogs() {
  // As the tray keeps them (FormatLogLine): time, level, the core's message.
  const std::vector<std::string> lines = {
      "13:42:02  INFO   outbound/anytls[AnyTLS-REALITY]: outbound connection to www.gstatic.com:443",
      "13:42:38  INFO   [2642219761 3ms] outbound/direct[direct]: outbound connection to vk.com:443",
      "13:43:08  INFO   [4168576375 3ms] outbound/anytls[AnyTLS-REALITY · aeza]: outbound connection to 140.82.121.4:443",
      "13:43:09  INFO   [1 2ms] outbound/vless[REALITY]: outbound connection to [2606:4700::6810:84e5]:443",
  };
  CHECK(OutboundInLogs(lines, {"vk.com", "87.240.132.72"}) == std::optional<std::string>("direct"));
  // In TUN the destination is the address the program connected to.
  CHECK(OutboundInLogs(lines, {"github.com", "140.82.121.4"}) == std::optional<std::string>("AnyTLS-REALITY · aeza"));
  CHECK(OutboundInLogs(lines, {"x.com", "2606:4700::6810:84e5"}) == std::optional<std::string>("REALITY"));
  CHECK(!OutboundInLogs(lines, {"example.com"}));
  CHECK(!OutboundInLogs(lines, {"vk.co"}));  // not a prefix of another name
}

void TestRules() {
  RoutingSettings routing;
  routing.rules.push_back(*ParseRule("qwen.ai, alicdn.com ~tracker 10.0.0.1", RouteRule::Action::Direct));
  CHECK(ClientRuleFor("chat.qwen.ai", routing).find("напрямую") != std::string::npos);
  CHECK(ClientRuleFor("my-tracker.example", routing).find("qwen.ai") != std::string::npos);
  CHECK(ClientRuleFor("10.0.0.1", routing).find("qwen.ai") != std::string::npos);
  CHECK(ClientRuleFor("notqwen.ai", routing).empty());  // a whole label, not a tail of one
  CHECK(ClientRuleFor("ya.ru", routing) == "российская зона .ru → напрямую");
  routing.russiaDirect = false;
  CHECK(ClientRuleFor("ya.ru", routing).empty());
  CHECK(Mbps(25'000'000, 2.0) == 100.0);
  CHECK(Mbps(1, 0) == 0);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestTraceAndIp();
    TestStun();
    TestDnsLeak();
    TestLogs();
    TestRules();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
