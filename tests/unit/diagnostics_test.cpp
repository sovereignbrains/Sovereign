// The checks' pure parts (src/tray/diagnostics.h): what's read off the wire
// and the log, and how results are judged.

#include <exception>
#include <iostream>
#include <optional>
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

void TestRoutes() {
  // The service's answer: a way, or none (no "outbound"), or not box_route's.
  const auto vpn = ParseRouteAnswer(
      R"({"cmd":"box_route","outbound":"AnyTLS · aeza","type":"anytls","chain":["AnyTLS · aeza",5,"proxy"]})");
  CHECK(vpn && vpn->outbound == "AnyTLS · aeza" && vpn->rule.empty());
  CHECK(vpn && vpn->chain == std::vector<std::string>({"AnyTLS · aeza", "proxy"}));
  CHECK(vpn && WayText(*vpn) == "через VPN");
  CHECK(!ParseRouteAnswer(R"({"cmd":"box_route"})"));
  CHECK(!ParseRouteAnswer(R"({"cmd":"error","message":"gocore not loaded"})"));
  CHECK(!ParseRouteAnswer("not json"));
  const RouteWay direct{.outbound = "direct", .type = "direct", .chain = {"direct"}, .rule = "rule_set=geosite-ru"};
  CHECK(WayText(direct) == "напрямую");
  CHECK(WayText(RouteWay{.outbound = "warp 2", .type = "wireguard", .chain = {}, .rule = {}}) == "через WARP");
  CHECK(WayText(RouteWay{.outbound = "Home WG", .type = "wireguard", .chain = {}, .rule = {}}) == "через VPN");
  // The bucket a checked address went to - what the "Пустить иначе" buttons leave out.
  const auto action = [](RouteRule::Action a) { return static_cast<int>(a); };
  CHECK(WayAction(direct) == action(RouteRule::Action::Direct));
  CHECK(WayAction(RouteWay{.outbound = "block", .type = "block", .chain = {}, .rule = {}}) == action(RouteRule::Action::Block));
  CHECK(WayAction(RouteWay{.outbound = "warp 2", .type = "wireguard", .chain = {}, .rule = {}}) == action(RouteRule::Action::Warp));
  CHECK(WayAction(RouteWay{.outbound = "Home WG", .type = "wireguard", .chain = {}, .rule = {}}) == action(RouteRule::Action::Proxy));
  CHECK(WayAction(RouteWay{.outbound = "AnyTLS · aeza", .type = "anytls", .chain = {}, .rule = {}}) == action(RouteRule::Action::Proxy));

  RoutingSettings routing;  // the Russian sites directly
  const auto site = [](const char* host, bool russian, std::optional<RouteWay> way) {
    return SiteWay{.host = host, .where = {}, .russian = russian, .resolved = true, .way = std::move(way)};
  };
  // As it should be: one line by outcome, in the sites' order.
  CheckResult r = JudgeRoutes({site("ya.ru", true, direct), site("spiegel.de", false, vpn), site("chatgpt.com", false, vpn)},
                              routing);
  CHECK(r.status == CheckResult::Status::Ok);
  CHECK(r.summary == "ya.ru → напрямую · spiegel.de, chatgpt.com → через VPN");
  CHECK(r.detail.find("chatgpt.com — через VPN (AnyTLS · aeza)") != std::string::npos);
  CHECK(r.detail.find("ya.ru — напрямую; российская зона .ru → напрямую") != std::string::npos);

  // A foreign site directly: a failure - unless the user's own rule says so.
  r = JudgeRoutes({site("ya.ru", true, direct), site("spiegel.de", false, direct)}, routing);
  CHECK(r.status == CheckResult::Status::Fail);
  CHECK(r.detail.starts_with("spiegel.de идёт мимо VPN"));
  routing.rules.push_back(*ParseRule("spiegel.de", RouteRule::Action::Direct));
  CHECK(JudgeRoutes({site("spiegel.de", false, direct)}, routing).status == CheckResult::Status::Ok);
  routing.rules.clear();

  // Named by what they stand for, as "Проверить всё" sends them (kRouteProbes).
  {
    std::vector<SiteWay> probes;
    probes.reserve(kRouteProbes.size());
    for (const RouteProbe& probe : kRouteProbes) {
      probes.push_back(SiteWay{.host = std::string(probe.host),
                               .where = std::string(probe.where),
                               .russian = probe.russian,
                               .resolved = true,
                               .way = probe.russian ? std::optional<RouteWay>(direct) : vpn});
    }
    CHECK(JudgeRoutes(probes, routing).summary == "Россия → напрямую · Европа, США → через VPN");
  }
  // Only chosen things through the VPN: a foreign site directly is the mode, not a failure.
  r = JudgeRoutes({site("ya.ru", true, direct), site("spiegel.de", false, direct), site("chatgpt.com", false, direct)},
                  routing, /*selective=*/true);
  CHECK(r.status == CheckResult::Status::Ok);
  CHECK(r.summary == "ya.ru, spiegel.de, chatgpt.com → напрямую");
  CHECK(r.detail.starts_with("Через VPN идут только выбранные"));

  // The Russian one through the VPN: a warning with them directly, as it
  // should be with them through the VPN or of no way of their own; a site
  // not seen, a warning either way.
  r = JudgeRoutes({site("ya.ru", true, vpn)}, routing);
  CHECK(r.status == CheckResult::Status::Warn);
  CHECK(r.detail.starts_with("ya.ru идёт через VPN, а российским сайтам задан путь напрямую."));
  routing.russia = RouteRule::Action::Proxy;
  CHECK(JudgeRoutes({site("ya.ru", true, vpn)}, routing).status == CheckResult::Status::Ok);

  // Through WARP (23.10.2026): WARP's way is right, the VPN's warns; WARP off,
  // no way of their own - the VPN's is right.
  const RouteWay warp{.outbound = "warp", .type = "wireguard", .chain = {"warp"}, .rule = {}};
  routing.russia = RouteRule::Action::Warp;
  routing.warp = true;
  routing.warpAccount = WarpAccount{};
  r = JudgeRoutes({site("ya.ru", true, warp), site("chatgpt.com", false, vpn)}, routing);
  CHECK(r.status == CheckResult::Status::Ok);
  CHECK(r.detail.starts_with("Российские сайты идут через WARP"));
  CHECK(ClientRuleFor("ya.ru", routing) == "российская зона .ru → через WARP");
  CHECK(JudgeRoutes({site("ya.ru", true, vpn)}, routing).status == CheckResult::Status::Warn);
  routing.warp = false;
  CHECK(JudgeRoutes({site("ya.ru", true, vpn)}, routing).status == CheckResult::Status::Ok);
  CHECK(ClientRuleFor("ya.ru", routing).empty());

  // Blocked: no address, or no connection seen, is as meant; one through warns.
  routing.russia = RouteRule::Action::Block;
  r = JudgeRoutes({SiteWay{.host = "ya.ru", .where = "Россия", .russian = true, .resolved = false, .way = std::nullopt},
                   site("chatgpt.com", false, vpn)},
                  routing);
  CHECK(r.status == CheckResult::Status::Ok);
  CHECK(r.summary == "Россия → заблокировано · chatgpt.com → через VPN");
  CHECK(JudgeRoutes({site("ya.ru", true, std::nullopt)}, routing).status == CheckResult::Status::Ok);
  r = JudgeRoutes({site("ya.ru", true, direct)}, routing);
  CHECK(r.status == CheckResult::Status::Warn && r.detail.starts_with("ya.ru идёт напрямую, хотя российские сайты в блоке."));

  routing.russia.reset();
  CHECK(JudgeRoutes({site("ya.ru", true, vpn)}, routing).status == CheckResult::Status::Ok);
  r = JudgeRoutes({site("ya.ru", true, vpn), site("chatgpt.com", false, std::nullopt),
                   SiteWay{.host = "nx.example", .where = {}, .russian = false, .resolved = false, .way = std::nullopt}},
                  routing);
  CHECK(r.status == CheckResult::Status::Warn);
  CHECK(r.summary == "ya.ru → через VPN · chatgpt.com → не видно · nx.example → не находится");
}

void TestRules() {
  RoutingSettings routing;
  routing.rules.push_back(*ParseRule("qwen.ai, alicdn.com ~tracker 10.0.0.1", RouteRule::Action::Direct));
  CHECK(ClientRuleFor("chat.qwen.ai", routing).find("напрямую") != std::string::npos);
  CHECK(ClientRuleFor("my-tracker.example", routing).find("qwen.ai") != std::string::npos);
  CHECK(ClientRuleFor("10.0.0.1", routing).find("qwen.ai") != std::string::npos);
  CHECK(ClientRuleFor("notqwen.ai", routing).empty());  // a whole label, not a tail of one
  CHECK(ClientRuleFor("ya.ru", routing) == "российская зона .ru → напрямую");
  routing.russia = RouteRule::Action::Block;
  CHECK(ClientRuleFor("ya.ru", routing) == "российская зона .ru → блокировать");
  routing.russia.reset();
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
    TestRoutes();
    TestRules();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
