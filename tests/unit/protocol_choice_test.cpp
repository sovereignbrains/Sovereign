// Picking the proxy protocol (src/tray/protocol_choice.h).

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "protocol_choice.h"

namespace {

using nlohmann::json;
using namespace sovereign::tray;

// packetlab's shape: a selector "proxy" in front of a urltest "auto".
constexpr std::string_view kSubscription = R"({
  "outbounds": [
    {"type":"selector","tag":"proxy","outbounds":["auto","AnyTLS-REALITY","TUIC"],"default":"auto"},
    {"type":"urltest","tag":"auto","outbounds":["AnyTLS-REALITY","TUIC"]},
    {"type":"anytls","tag":"AnyTLS-REALITY"},{"type":"tuic","tag":"TUIC"},{"type":"direct","tag":"direct"}],
  "route": {"final":"proxy"}
})";

void TestFindsTheSelector() {
  const ProtocolChoices c = FindProtocolChoices(kSubscription);
  CHECK(c.selector == "proxy");
  CHECK(c.options == std::vector<std::string>({"auto", "AnyTLS-REALITY", "TUIC"}));
  CHECK(c.configDefault == "auto");
  CHECK(c.groups.size() == 1 && c.groups.contains("auto") &&
        c.groups.at("auto") == std::vector<std::string>({"AnyTLS-REALITY", "TUIC"}));
}

// A URL test only counts as one of the selector's options, and only with the
// servers the selector offers as well.
void TestGroups() {
  constexpr std::string_view config = R"({"outbounds":[
    {"type":"selector","tag":"proxy","outbounds":["auto","a"]},
    {"type":"urltest","tag":"auto","outbounds":["a","hidden",5]},
    {"type":"urltest","tag":"other","outbounds":["a"]},
    {"type":"urltest","tag":7}]})";
  const ProtocolChoices c = FindProtocolChoices(config);
  CHECK(c.groups.size() == 1 && c.groups.contains("auto") && c.groups.at("auto") == std::vector<std::string>({"a"}));
}

void TestPickSetsDefault() {
  const json c = json::parse(ApplyProtocolChoice(kSubscription, "TUIC"));
  CHECK(c["outbounds"][0]["default"] == "TUIC");
  CHECK(c["outbounds"][1]["type"] == "urltest");  // nothing else touched
}

void TestUnknownOrEmptyPickChangesNothing() {
  CHECK(json::parse(ApplyProtocolChoice(kSubscription, "")) == json::parse(kSubscription));
  // The subscription dropped the protocol the user had picked.
  CHECK(json::parse(ApplyProtocolChoice(kSubscription, "Hysteria2")) == json::parse(kSubscription));
}

void TestSelectorByFinalAndFallbacks() {
  // route.final names the second selector; without final the first one counts.
  constexpr std::string_view two =
      R"({"outbounds":[{"type":"selector","tag":"a","outbounds":["x"]},{"type":"selector","tag":"b","outbounds":["y","z"]}],"route":{"final":"b"}})";
  CHECK(FindProtocolChoices(two).selector == "b");
  CHECK(FindProtocolChoices(two).configDefault == "y");  // no default: the first option
  CHECK(FindProtocolChoices(R"({"outbounds":[{"type":"selector","tag":"a","outbounds":["x"]}]})").selector == "a");
  CHECK(FindProtocolChoices(R"({"outbounds":[{"type":"direct","tag":"d"}]})").selector.empty());
  CHECK(FindProtocolChoices("not json").options.empty());
  CHECK(ApplyProtocolChoice("not json", "x") == "not json");
}

// Fields of the wrong type (found by tests/fuzz) are as good as absent - never an exception.
void TestHostileTypes() {
  CHECK(FindProtocolChoices(R"({"outbounds":[{"type":"selector","tag":5,"outbounds":[1,"a"]}],"route":{"final":7}})").options ==
        std::vector<std::string>({"a"}));
  CHECK(FindProtocolChoices(R"({"outbounds":[{"type":"selector","tag":"p","outbounds":["a"],"default":3}]})").configDefault == "a");
  CHECK(FindProtocolChoices(R"({"outbounds":[{"type":5}]})").selector.empty());
  (void)ApplyProtocolChoice(R"({"outbounds":[{"type":"selector","tag":5,"outbounds":["a"]}]})", "a");
}

}  // namespace

// A server's title from what it is, whatever it's named.
void TestServerTitles() {
  const auto titles = ServerTitles(R"({"outbounds":[
    {"type":"urltest","tag":"auto","outbounds":["a"]},
    {"type":"anytls","tag":"🇩🇪 Germany","server":"de.example.com","server_port":443,
     "tls":{"enabled":true,"reality":{"enabled":true}}},
    {"type":"anytls","tag":"x","server":"de.example.com","tls":{"enabled":true,"ech":{"enabled":true}}},
    {"type":"hysteria2","tag":"Hy2","server":"1.2.3.4","tls":{"enabled":true}},
    {"type":"tuic","tag":"tuic-5"},
    {"type":"vless","tag":"v1","flow":"xtls-rprx-vision","tls":{"enabled":true,"reality":{"enabled":true}}},
    {"type":"vless","tag":"v2","transport":{"type":"ws"},"tls":{"enabled":true}},
    {"type":"vmess","tag":"v3","transport":{"type":"grpc"}},
    {"type":"shadowsocks","tag":"s1","method":"2022-blake3-aes-128-gcm"},
    {"type":"shadowsocks","tag":"s2","method":"chacha20-ietf-poly1305"},
    {"type":"trojan","tag":"t","tls":{"enabled":true}},
    {"type":"http","tag":"h","tls":{"enabled":true}},
    {"type":"selector","tag":"proxy","outbounds":["auto"]},{"type":"direct","tag":"direct"},
    {"type":"somethingnew","tag":"n"}],
   "endpoints":[{"type":"wireguard","tag":"wg","peers":[{"address":"wg.example.com","port":51820}]}]})");
  const auto title = [&](const std::string& tag) { return titles.contains(tag) ? titles.at(tag).title : "-"; };
  CHECK(title("auto") == "Авто");
  CHECK(title("🇩🇪 Germany") == "AnyTLS + REALITY" && titles.at("🇩🇪 Germany").host == "de.example.com" &&
        titles.at("🇩🇪 Germany").port == 443);
  CHECK(title("x") == "AnyTLS + ECH");
  CHECK(title("Hy2") == "Hysteria 2" && title("tuic-5") == "TUIC" && title("t") == "Trojan");
  CHECK(title("v1") == "VLESS + Vision + REALITY" && title("v2") == "VLESS + WebSocket + TLS");
  CHECK(title("v3") == "VMess + gRPC");
  CHECK(title("s1") == "Shadowsocks 2022" && title("s2") == "Shadowsocks" && title("h") == "HTTPS");
  CHECK(title("wg") == "WireGuard" && titles.at("wg").host == "wg.example.com");
  CHECK(title("proxy") == "-" && title("direct") == "-" && title("n") == "-");
  CHECK(ServerTitles("not json").empty());
  CHECK(NameIsTitle("AnyTLS-REALITY", "AnyTLS + REALITY") && NameIsTitle("hysteria2", "Hysteria 2"));
  CHECK(!NameIsTitle("🇪🇺 4G | Whitelist №2", "VLESS + Vision + REALITY") && !NameIsTitle("", ""));
  // Protocol words, a country code and numbers only: nothing the title doesn't say (29.10.2026, the servers page).
  CHECK(NameIsTitle("REALITY", "VLESS + Vision + REALITY") && NameIsTitle("NL-Naive", "NaïveProxy") &&
        NameIsTitle("AnyTLS-REALITY 2", "AnyTLS + REALITY") && NameIsTitle("🇳🇱 NL-TUIC", "TUIC"));
  CHECK(!NameIsTitle("Германия", "Hysteria 2") && !NameIsTitle("DE Frankfurt Hysteria2", "Hysteria 2"));
}

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestFindsTheSelector();
    TestPickSetsDefault();
    TestGroups();
    TestUnknownOrEmptyPickChangesNothing();
    TestSelectorByFinalAndFallbacks();
    TestHostileTypes();
    TestServerTitles();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
