// The report for a subscription's support (src/tray/support_report.h): what
// it says about each server, and what it never says - keys, the
// subscription's link, the user's address.

#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "support_report.h"

namespace {

using namespace sovereign::tray;

// `--print`: the sample report on stdout, to read it as support would.
bool& Print() {
  static bool print = false;
  return print;
}

void TestFlagCountry() {
  CHECK(FlagCountry("\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1 Amsterdam") == "NL");
  CHECK(FlagCountry("Server \xF0\x9F\x87\xA9\xF0\x9F\x87\xAA") == "DE");
  CHECK(FlagCountry("AnyTLS-REALITY").empty());
  CHECK(FlagCountry("\xF0\x9F\x87\xB3").empty());  // half a flag
}

void TestCutAddresses() {
  const std::string cut = CutAddresses("write tcp 192.168.31.57:5555->5.83.147.210:443: broken pipe", "46.148.140.142");
  CHECK(cut == "write tcp <...>->5.83.147.210:443: broken pipe");  // the PC's own address out, the server's kept
  CHECK(CutAddresses("from 46.148.140.142:50000 refused", "46.148.140.142") == "from <...> refused");
  CHECK(CutAddresses("dial [fe80::1]:53 and [2001:db8::5]:443", "") == "dial <...> and [2001:db8::5]:443");
  CHECK(CutAddresses("me 2a00:1:2::7 here", "2a00:1:2::7") == "me <...> here");
  CHECK(CutAddresses("10.0.0.1 100.64.1.2 172.20.0.1 8.8.8.8", "") == "<...> <...> <...> 8.8.8.8");
}

void TestServers() {
  const auto servers = ReportServers(R"({
    "outbounds": [
      {"type": "selector", "tag": "proxy", "outbounds": ["NL"]},
      {"type": "vless", "tag": "NL", "server": "nl.example.com", "server_port": 443,
       "uuid": "11111111-2222-3333-4444-555555555555",
       "tls": {"enabled": true, "reality": {"enabled": true, "public_key": "PUBKEY", "short_id": "abcd"}}},
      {"type": "vmess", "tag": "DE", "server": "de.example.com", "server_port": 8443, "uuid": "u",
       "transport": {"type": "ws", "path": "/secret-path"}, "tls": {"enabled": true}},
      {"type": "hysteria2", "tag": "HY", "server": "2001:db8::9", "server_port": 0, "password": "hunter2",
       "obfs": {"type": "salamander", "password": "obfs-secret"}, "tls": {"enabled": true}},
      {"type": "direct", "tag": "direct"}],
    "endpoints": [{"type": "wireguard", "tag": "WG", "private_key": "PRIVKEY",
                   "peers": [{"address": "wg.example.com", "port": 51820, "public_key": "PEERKEY"}]}]
  })");
  CHECK(servers.size() == 4);
  if (servers.size() == 4) {
    CHECK(servers[0].name == "NL" && servers[0].protocol == "vless · REALITY" && servers[0].host == "nl.example.com" &&
          servers[0].port == 443);
    CHECK(servers[1].protocol == "vmess · ws · TLS" && servers[1].port == 8443);
    CHECK(servers[2].protocol == "hysteria2 · TLS · salamander");
    CHECK(servers[3].name == "WG" && servers[3].host == "wg.example.com" && servers[3].port == 51820);
  }
  for (const ReportServer& s : servers) {
    const std::string all = s.name + s.protocol + s.host;
    for (const char* secret : {"1111", "PUBKEY", "abcd", "secret", "hunter2", "PRIVKEY", "PEERKEY"}) {
      CHECK(all.find(secret) == std::string::npos);
    }
  }
  CHECK(ReportServers("not json").empty());
}

ReportServer Failed(const char* error) {
  ReportServer s;
  s.name = "S";
  s.state = ReportServer::State::Failed;
  s.error = error;
  return s;
}

void TestProblems() {
  CHECK(ServerProblem(Failed("inbound/anytls: unknown user password")).find("ключ") != std::string::npos);
  CHECK(ServerProblem(Failed("reality verification failed")).find("устарела") != std::string::npos);
  CHECK(ServerProblem(Failed("dial tcp 1.2.3.4:443: i/o timeout")).find("не отвечает") != std::string::npos);
  CHECK(ServerProblem(Failed("failed to create session: EOF")).find("обрывается") != std::string::npos);
  CHECK(ServerProblem(Failed("connectex: No connection could be made because the target machine actively refused it"))
            .find("порт закрыт") != std::string::npos);
  CHECK(ServerProblem(Failed("tls: x509: certificate has expired")).find("сертификат") != std::string::npos);
  CHECK(ServerProblem(Failed("lookup nope.example: no such host")).find("DNS") != std::string::npos);
  // A lookup that timed out: the user's network, not the provider's domain.
  CHECK(ServerProblem(Failed("lookup packetlab.tech: context deadline exceeded")).find("у пользователя") !=
        std::string::npos);
  CHECK(ServerProblem(Failed("something new")) == "не подключается");

  ReportServer ok;
  ok.name = "\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1 NL";
  ok.state = ReportServer::State::Ok;
  ok.delay = 60;
  CHECK(ServerProblem(ok).empty());
  ok.exitCountry = "NL";
  CHECK(ServerProblem(ok).empty());
  ok.exitCountry = "DE";
  CHECK(ServerProblem(ok).find("DE") != std::string::npos);  // named NL, comes out in DE
  ok.exitCountry = "NL";
  ok.loss = 35;
  CHECK(ServerProblem(ok).find("35 %") != std::string::npos);
  ok.loss = 0;
  ok.delay = 1500;
  CHECK(ServerProblem(ok).find("1500 мс") != std::string::npos);
  CHECK(ServerProblem(ReportServer{}).empty());  // not measured: nothing claimed
}

void TestReport() {
  ReportInput in;
  in.now = 1791300000;
  in.subscriptionName = "packetlab.tech";
  in.subscriptionHost = "packetlab.tech";
  in.lastRefresh = 1791096133;
  in.trafficUsed = 3145728;
  in.trafficTotal = 107374182400ULL;
  in.expire = 1798761600;
  in.connected = true;
  in.userIp = "46.148.140.142";
  ReportServer good;
  good.name = "AnyTLS-REALITY";
  good.protocol = "anytls · REALITY";
  good.host = "packetlab.tech";
  good.port = 443;
  good.state = ReportServer::State::Ok;
  good.delay = 54;
  good.jitter = 1;
  good.exitIp = "5.83.147.210";
  good.exitCountry = "DE";
  ReportServer bad = Failed("dial tcp 192.168.31.57:61000->5.83.147.210:443 from 46.148.140.142: unknown user");
  bad.name = "AnyTLS-ECH";
  bad.protocol = "anytls · TLS · ECH";
  in.servers = {good, bad};

  const SupportReport r = BuildSupportReport(in);
  if (Print()) {
    std::cout << r.full << "\n----- brief -----\n" << r.brief << "\n";
  }
  CHECK(r.full.find("работает 1 из 2 серверов") != std::string::npos);
  CHECK(r.full.find("AnyTLS-ECH — сервер отклоняет ключ") != std::string::npos);
  CHECK(r.full.find("3 МБ из 100,0 ГБ") != std::string::npos);
  CHECK(r.full.find("UTC") != std::string::npos && r.full.find("UTC+") == std::string::npos);  // not the user's zone
  CHECK(r.full.find("задержка 54 мс") != std::string::npos);
  // Nothing of the client, the system or the user's network: it doesn't help
  // them fix a server, it tells them about the user.
  for (const char* none : {"Sovereign", "sing-box", "Windows", "Trytek", "RU", "Утечка", "пользователя:"}) {
    CHECK(r.full.find(none) == std::string::npos);
    CHECK(r.brief.find(none) == std::string::npos);
  }
  CHECK(r.full.find("46.148.140.142") == std::string::npos);  // the user's address: nowhere
  CHECK(r.full.find("192.168.31.57") == std::string::npos);
  CHECK(r.full.find("5.83.147.210") != std::string::npos);    // the server's: there
  CHECK(r.brief.find("packetlab.tech") != std::string::npos && r.brief.find("AnyTLS-ECH") != std::string::npos);
  CHECK(r.brief.find("46.148.140.142") == std::string::npos);

  // How sure the report is a server is down, and where it came out - before.
  ReportInput fails = in;
  ReportServer gone = Failed("i/o timeout");
  gone.name = "TUIC";
  gone.exitIp = "5.83.147.210";
  gone.exitCountry = "DE";
  gone.failedInRow = 1;
  fails.servers = {good, gone};
  std::string text = BuildSupportReport(fails).full;
  CHECK(text.find("НЕ РАБОТАЕТ на последней проверке") != std::string::npos);
  CHECK(text.find("Последний известный выход: 5.83.147.210 · DE") != std::string::npos);
  CHECK(text.find("Выход в интернет: 5.83.147.210 · DE") != std::string::npos);  // the one that works: now
  fails.servers[1].failedInRow = 3;
  text = BuildSupportReport(fails).full;
  CHECK(text.find("не ответил на 3 проверки подряд") != std::string::npos);
  fails.servers[1].failedInRow = 5;
  CHECK(BuildSupportReport(fails).full.find("на 5 проверок подряд") != std::string::npos);

  // Nothing measured: nothing claimed about the servers.
  ReportInput off = in;
  off.connected = false;
  ReportServer a;
  a.name = "A";
  ReportServer b;
  b.name = "B";
  off.servers = {a, b};
  CHECK(BuildSupportReport(off).full.find("не проверены") != std::string::npos);

  // All down: maybe the user's side.
  ReportInput down = in;
  down.servers = {Failed("i/o timeout"), Failed("i/o timeout")};
  CHECK(BuildSupportReport(down).full.find("ни один") != std::string::npos);

  // Over the paid period, out of traffic, a failing refresh: said up front.
  ReportInput expired = in;
  expired.expire = in.now - 86400;
  expired.trafficUsed = expired.trafficTotal;
  expired.refreshError = "сервер подписки ответил 403";
  const std::string head = BuildSupportReport(expired).full.substr(0, 600);
  CHECK(head.find("срок подписки истёк") != std::string::npos && head.find("трафик исчерпан") != std::string::npos &&
        head.find("не обновляется") != std::string::npos);
}

}  // namespace

void TestTrouble() {
  constexpr std::int64_t kNow = 1791300000;
  CHECK(SubscriptionTrouble(3, 3, 0, false, 0, 0, 0, kNow).empty());           // fine: nothing on the screen
  CHECK(SubscriptionTrouble(3, 3, 1, false, 0, 0, 0, kNow).empty());           // one of three: not worth a line
  CHECK(SubscriptionTrouble(4, 4, 2, false, 0, 0, 0, kNow) == "не работает 2 из 4 серверов");
  CHECK(SubscriptionTrouble(2, 2, 2, false, 0, 0, 0, kNow) == "не отвечает ни один сервер");
  CHECK(SubscriptionTrouble(1, 1, 1, false, 0, 0, 0, kNow) == "сервер не отвечает");
  CHECK(SubscriptionTrouble(3, 0, 0, false, 0, 0, 0, kNow).empty());           // not measured: nothing claimed
  CHECK(SubscriptionTrouble(3, 3, 0, true, 0, 0, 0, kNow) == "подписка не обновляется");
  CHECK(SubscriptionTrouble(3, 3, 3, true, 10, 10, kNow - 1, kNow) == "оплата истекла");  // the worst one
  CHECK(SubscriptionTrouble(3, 3, 0, false, 10, 10, 0, kNow) == "трафик закончился");
}

void TestUsageLine() {
  CHECK(UsageLine(3145728, 107374182400ULL, 1798761600, 1791300000, 180) == "Трафик: 3 МБ из 100,0 ГБ · оплачено до 01.01.2027");
  CHECK(UsageLine(1610612736, 0, 0, 0, 0) == "Трафик: 1,5 ГБ (без лимита)");
  CHECK(UsageLine(0, 0, 1700000000, 1791300000, 0) == "оплата истекла 14.11.2023");
  CHECK(UsageLine(0, 0, 0, 1791300000, 180).empty());
}

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  Print() = argc == 2 && std::string(argv[1]) == "--print";
  try {
    TestFlagCountry();
    TestCutAddresses();
    TestServers();
    TestProblems();
    TestReport();
    TestUsageLine();
    TestTrouble();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
