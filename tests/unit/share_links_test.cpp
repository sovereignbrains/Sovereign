// Share links and what the user pastes (src/tray/share_links.h). The keys
// here are made up: UUIDs, passwords and REALITY keys of no server.

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "share_links.h"

namespace {

using nlohmann::json;
using sovereign::tray::BuildConfigFromLinks;
using sovereign::tray::DecodeBase64;
using sovereign::tray::EncodeBase64;
using sovereign::tray::ExtractShareLinks;
using sovereign::tray::ParseShareLink;
using sovereign::tray::RecognizeImport;

constexpr std::string_view kVless =
    "vless://11111111-2222-3333-4444-555555555555@nl.example.com:443?type=tcp&security=reality&sni=www.microsoft.com"
    "&fp=chrome&pbk=AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE&sid=6ba85179e30d4fc2&flow=xtls-rprx-vision"
    "#%F0%9F%87%B3%F0%9F%87%B1%20Netherlands";
constexpr std::string_view kTrojan = "trojan://p%40ss@de.example.com:8443?type=ws&path=%2Fws%3Fed%3D2048&host=cdn.example.com#DE";
constexpr std::string_view kSs = "ss://YWVzLTI1Ni1nY206c2VjcmV0@1.2.3.4:8388#SS";
constexpr std::string_view kHy2 = "hy2://pass@hy.example.com:443?sni=hy.example.com&obfs=salamander&obfs-password=ob&insecure=1#HY";
constexpr std::string_view kTuic =
    "tuic://11111111-2222-3333-4444-555555555555:pw@tuic.example.com:443?congestion_control=bbr&alpn=h3#TUIC";

json Outbound(std::string_view link) {
  const auto parsed = ParseShareLink(link);
  if (!parsed.outbound) {
    std::cerr << "  " << link << ": " << parsed.error << "\n";
    return json();
  }
  return json::parse(*parsed.outbound);
}

void TestBase64() {
  CHECK(DecodeBase64("aGVsbG8=") == std::optional<std::string>("hello"));
  CHECK(DecodeBase64("aGVsbG8") == std::optional<std::string>("hello"));  // no padding
  CHECK(DecodeBase64("a GVs\nbG8=") == std::optional<std::string>("hello"));
  CHECK(DecodeBase64("-_8") == DecodeBase64("+/8"));  // URL-safe
  CHECK(!DecodeBase64("not base64!"));
  CHECK(!DecodeBase64(""));
  CHECK(EncodeBase64("hello") == "aGVsbG8=");
  CHECK(EncodeBase64("hi") == "aGk=");
  CHECK(DecodeBase64(EncodeBase64("\x01\xFF\x80xyz")) == std::optional<std::string>("\x01\xFF\x80xyz"));
}

void TestVless() {
  const json o = Outbound(kVless);
  CHECK(o["type"] == "vless");
  CHECK(o["server"] == "nl.example.com");
  CHECK(o["server_port"] == 443);
  CHECK(o["uuid"] == "11111111-2222-3333-4444-555555555555");
  CHECK(o["flow"] == "xtls-rprx-vision");
  CHECK(o["tls"]["server_name"] == "www.microsoft.com");
  CHECK(o["tls"]["reality"]["public_key"] == "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE");
  CHECK(o["tls"]["reality"]["short_id"] == "6ba85179e30d4fc2");
  CHECK(o["tls"]["utls"]["fingerprint"] == "chrome");
  CHECK(!o.contains("transport"));
  CHECK(ParseShareLink(kVless).name == "\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1 Netherlands");

  const json grpc = Outbound("vless://id@[2001:db8::1]:443?type=grpc&serviceName=svc&security=tls&sni=a.b");
  CHECK(grpc["server"] == "2001:db8::1");
  CHECK(grpc["transport"]["type"] == "grpc");
  CHECK(grpc["transport"]["service_name"] == "svc");
  CHECK(grpc["tls"]["enabled"] == true);
  CHECK(ParseShareLink("vless://id@[2001:db8::1]:443").name == "[2001:db8::1]:443");  // no name: the address

  CHECK(!ParseShareLink("vless://id@host:443?type=xhttp").outbound);  // sing-box has no xhttp
  CHECK(!ParseShareLink("vless://id@host").outbound);                 // no port
  CHECK(!ParseShareLink("vless://@host:443").outbound);               // no UUID
}

void TestTrojanWs() {
  const json o = Outbound(kTrojan);
  CHECK(o["type"] == "trojan");
  CHECK(o["password"] == "p@ss");
  CHECK(o["tls"]["enabled"] == true);  // trojan is TLS unless it says otherwise
  CHECK(o["transport"]["type"] == "ws");
  CHECK(o["transport"]["path"] == "/ws");
  CHECK(o["transport"]["max_early_data"] == 2048);
  CHECK(o["transport"]["headers"]["Host"] == "cdn.example.com");
}

void TestShadowsocks() {
  const json sip002 = Outbound(kSs);
  CHECK(sip002["method"] == "aes-256-gcm");
  CHECK(sip002["password"] == "secret");
  CHECK(sip002["server"] == "1.2.3.4");
  // 2022 methods: plain, percent-encoded userinfo (the key is base64 itself).
  const json plain = Outbound("ss://2022-blake3-aes-128-gcm:a2V5MTIzNDU2Nzg5MDEyMw%3D%3D@h.example.com:443");
  CHECK(plain["method"] == "2022-blake3-aes-128-gcm");
  CHECK(plain["password"] == "a2V5MTIzNDU2Nzg5MDEyMw==");
  // The old form: all of it base64.
  const json legacy = Outbound("ss://" + EncodeBase64("chacha20-ietf-poly1305:pw@5.6.7.8:9000") + "#old");
  CHECK(legacy["method"] == "chacha20-ietf-poly1305");
  CHECK(legacy["server_port"] == 9000);
  const json plugin = Outbound("ss://YWVzLTI1Ni1nY206c2VjcmV0@1.2.3.4:8388/?plugin=obfs-local%3Bobfs%3Dhttp%3Bobfs-host%3Dx.com");
  CHECK(plugin["plugin"] == "obfs-local");
  CHECK(plugin["plugin_opts"] == "obfs=http;obfs-host=x.com");
}

void TestVmess() {
  const std::string body =
      R"({"v":"2","ps":"VM","add":"vm.example.com","port":"443","id":"11111111-2222-3333-4444-555555555555",)"
      R"("aid":"0","scy":"auto","net":"ws","type":"none","host":"vm.example.com","path":"/v","tls":"tls","sni":"vm.example.com"})";
  const auto parsed = ParseShareLink("vmess://" + EncodeBase64(body));
  CHECK(parsed.outbound.has_value());
  CHECK(parsed.name == "VM");
  const json o = json::parse(parsed.outbound.value_or("{}"));
  CHECK(o["type"] == "vmess");
  CHECK(o["server_port"] == 443);
  CHECK(o["transport"]["path"] == "/v");
  CHECK(o["tls"]["server_name"] == "vm.example.com");
  CHECK(!o.contains("tag"));
  CHECK(!ParseShareLink("vmess://" + EncodeBase64(R"({"add":"x","port":"1"})")).outbound);  // no id
}

void TestHysteria2AndTuic() {
  const json hy = Outbound(kHy2);
  CHECK(hy["type"] == "hysteria2");
  CHECK(hy["password"] == "pass");
  CHECK(hy["obfs"]["type"] == "salamander");
  CHECK(hy["tls"]["insecure"] == true);
  const json hop = Outbound("hysteria2://p@h.example.com:443?mport=20000-30000");
  CHECK(hop["server_ports"] == json::array({"20000:30000"}));
  const json multi = Outbound("hysteria2://p@h.example.com:443,5000-6000");
  CHECK(multi["server_ports"] == json::array({"443:443", "5000:6000"}));

  const json tuic = Outbound(kTuic);
  CHECK(tuic["uuid"] == "11111111-2222-3333-4444-555555555555");
  CHECK(tuic["password"] == "pw");
  CHECK(tuic["congestion_control"] == "bbr");
  CHECK(tuic["tls"]["alpn"] == json::array({"h3"}));
}

// However the keys come, each comes out whole.
void TestExtraction() {
  const std::vector<std::string> all = {std::string(kVless), std::string(kTrojan), std::string(kSs),
                                        std::string(kHy2), std::string(kTuic)};
  std::string lines;
  std::string spaced;
  std::string glued;
  std::string commas;
  for (const auto& link : all) {
    lines += link + "\r\n";
    spaced += link + " ";
    glued += link;
    commas += link + ",";
  }
  CHECK(ExtractShareLinks(lines) == all);
  CHECK(ExtractShareLinks(glued) == all);
  CHECK(ExtractShareLinks(commas) == all);
  CHECK(ExtractShareLinks(EncodeBase64(lines)) == all);  // a subscription's base64 list
  CHECK(ExtractShareLinks("вот ключи: " + spaced + " пользуйся").size() == all.size());

  // A name with spaces survives; the next key ends it.
  const auto named = ExtractShareLinks("trojan://pw@a.com:443#My Server 1 trojan://pw@b.com:443#Two");
  CHECK(named.size() == 2);
  CHECK(!named.empty() && ParseShareLink(named[0]).name == "My Server 1");
  // A web link inside a key's parameter is part of the key.
  const auto inner = ExtractShareLinks("vless://id@h.com:443?spx=https://x.com/a#N");
  CHECK(inner.size() == 1 && inner[0] == "vless://id@h.com:443?spx=https://x.com/a#N");
}

void TestRecognize() {
  const auto config = RecognizeImport("\xEF\xBB\xBF  {\"outbounds\":[{\"type\":\"direct\"}]}\n");
  CHECK(config.json.has_value());
  CHECK(config.urls.empty() && config.links.empty());

  const auto broken = RecognizeImport("{\"outbounds\": [\n}");
  CHECK(!broken.json && !broken.jsonError.empty());

  const auto mixed = RecognizeImport("https://sub.example.com/abc?x=1\n" + std::string(kVless) + "\nhttps://other.example.com/s");
  CHECK(mixed.urls == std::vector<std::string>({"https://sub.example.com/abc?x=1", "https://other.example.com/s"}));
  CHECK(mixed.links == std::vector<std::string>({std::string(kVless)}));

  CHECK(RecognizeImport("просто текст").Empty());
  CHECK(RecognizeImport("").Empty());
}

void TestBuildConfig() {
  const auto built = BuildConfigFromLinks({std::string(kVless), std::string(kTrojan), "vless://id@host:1?type=xhttp", std::string(kVless)});
  CHECK(built.servers == 3);
  CHECK(built.errors.size() == 1 && built.errors[0].starts_with("ключ 3:"));
  const json config = json::parse(built.config.value_or("{}"));
  const json& outbounds = config["outbounds"];
  CHECK(outbounds[0]["tag"] == "proxy" && outbounds[0]["default"] == "auto");
  CHECK(outbounds[1]["tag"] == "auto" && outbounds[1]["outbounds"].size() == 3);
  // The same name twice: numbered.
  CHECK(outbounds[4]["tag"] == "\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1 Netherlands 2");
  CHECK(config["route"]["final"] == "proxy");

  const auto one = BuildConfigFromLinks({std::string(kSs)});
  const json single = json::parse(one.config.value_or("{}"));
  CHECK(single["outbounds"][0]["outbounds"] == json::array({"SS"}));  // no "auto" for one server

  CHECK(!BuildConfigFromLinks({"vless://x"}).config);
  CHECK(!BuildConfigFromLinks({}).config);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestBase64();
    TestVless();
    TestTrojanWs();
    TestShadowsocks();
    TestVmess();
    TestHysteria2AndTuic();
    TestExtraction();
    TestRecognize();
    TestBuildConfig();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
