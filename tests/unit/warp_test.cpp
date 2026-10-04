// Cloudflare WARP (src/tray/warp.h, x25519.h): the key against RFC 7748's
// vectors, the API's answer read (a real one, its keys and token swapped),
// the account kept in tray.json and the endpoint sing-box gets.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "check.h"
#include "share_links.h"
#include "warp.h"
#include "x25519.h"

namespace {

using namespace sovereign::tray;

X25519Key Hex(std::string_view hex) {
  X25519Key key{};
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::uint8_t>(nibble(hex[2 * i]) * 16 + nibble(hex[2 * i + 1]));
  }
  return key;
}

void TestX25519() {
  // RFC 7748 6.1: Alice and Bob.
  const X25519Key alice = Hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  const X25519Key bob = Hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
  CHECK(X25519Public(alice) == Hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
  CHECK(X25519Public(bob) == Hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
  const X25519Key shared = Hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  CHECK(X25519(alice, X25519Public(bob)) == shared);
  CHECK(X25519(bob, X25519Public(alice)) == shared);
  // RFC 7748 5.2: the first scalar-multiplication vector.
  CHECK(X25519(Hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
               Hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")) ==
        Hex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));
}

// What api.cloudflareclient.com answered on 02.10.2026, trimmed, with keys
// and the token swapped for made-up ones.
constexpr std::string_view kAnswer = R"({
  "id": "675ff0e5-cc28-4921-a55c-831d0503d4f2", "type": "a", "model": "PC",
  "key": "QHSV2GnFL/oLe8xls4gu5GEnH0nF6PqRAwkL8zuIEJ4=",
  "account": {"id": "f8e12387-ef32-4326-ada3-554cdeca276a", "account_type": "free", "license": "x"},
  "config": {
    "client_id": "Ie4Q",
    "peers": [{"public_key": "bmXOC+F1FxEMF9dyiK2H5/1SUtzH0JuVo51h2wPfgyo=",
               "endpoint": {"v4": "162.159.192.2:0", "v6": "[2606:4700:d0::a29f:c002]:0",
                            "host": "engage.cloudflareclient.com:2408", "ports": [2408, 500, 1701, 4500]}}],
    "interface": {"addresses": {"v4": "172.16.0.2", "v6": "2606:4700:110:87bf:c0a0:2c80:262b:5924"}},
    "services": {"http_proxy": "172.16.0.1:2480"}
  },
  "token": "made-up-token", "warp_enabled": false, "tos": "2026-10-02T11:14:54.22Z", "enabled": true
})";

const std::string kPrivate = "YJ9Lx3UFbVd6R+1QpYx0K5nV5+OYn5cB4m3Hk4qH0lI=";

void TestRegistration() {
  const std::string body = WarpRegisterBody("QHSV2GnFL/oLe8xls4gu5GEnH0nF6PqRAwkL8zuIEJ4=", "2026-10-02T11:14:54.220Z");
  const auto json = nlohmann::json::parse(body);
  CHECK(json["key"] == "QHSV2GnFL/oLe8xls4gu5GEnH0nF6PqRAwkL8zuIEJ4=" && json["tos"] == "2026-10-02T11:14:54.220Z");
  CHECK(json["model"] == "PC" && json["locale"] == "en_US");

  const auto account = ParseWarpRegistration(kAnswer, kPrivate);
  CHECK(account.has_value());
  if (account) {
    CHECK(account->privateKey == kPrivate && account->id == "675ff0e5-cc28-4921-a55c-831d0503d4f2");
    CHECK(account->token == "made-up-token");
    CHECK(account->address4 == "172.16.0.2" && account->address6 == "2606:4700:110:87bf:c0a0:2c80:262b:5924");
    CHECK(account->peerKey == "bmXOC+F1FxEMF9dyiK2H5/1SUtzH0JuVo51h2wPfgyo=");
    CHECK(account->host == "engage.cloudflareclient.com" && account->port == 2408);
    // "Ie4Q" = 0x21 0xEE 0x10
    CHECK((account->reserved == std::array<int, 3>{0x21, 0xEE, 0x10}));

    // tray.json and back.
    const auto back = WarpFromJson(nlohmann::json::parse(WarpToJson(*account).dump()));
    CHECK(back && back->privateKey == kPrivate && back->reserved == account->reserved && back->port == 2408 &&
          back->address6 == account->address6);

    const auto endpoint = WarpEndpoint(*account, "warp", "proxy");
    CHECK(endpoint["type"] == "wireguard" && endpoint["tag"] == "warp" && endpoint["detour"] == "proxy");
    CHECK(endpoint["address"].size() == 2 && endpoint["address"][0] == "172.16.0.2/32");
    CHECK(endpoint["peers"][0]["address"] == "engage.cloudflareclient.com" && endpoint["peers"][0]["port"] == 2408);
    CHECK(endpoint["peers"][0]["reserved"] == "Ie4Q");  // {0x21, 0xEE, 0x10}: []uint8 in base64, as Go writes it
    CHECK(!WarpEndpoint(*account, "warp", "").contains("detour"));
  // Kept alive: over the proxy an idle tunnel's UDP session was closed every 5 min.
  CHECK(endpoint["peers"][0]["persistent_keepalive_interval"] == 25);
  }

  // What isn't an account.
  CHECK(!ParseWarpRegistration(kAnswer, "short"));
  CHECK(!ParseWarpRegistration("not json", kPrivate));
  CHECK(!ParseWarpRegistration(R"({"config":{"peers":[]}})", kPrivate));
  CHECK(!ParseWarpRegistration(
      R"({"config":{"peers":[{"public_key":"bmXOC+F1FxEMF9dyiK2H5/1SUtzH0JuVo51h2wPfgyo="}],)"
      R"("interface":{"addresses":{"v4":"<script>"}}}})",
      kPrivate));
  CHECK(!WarpFromJson(nlohmann::json::parse(R"({"privateKey":"x"})")));
  CHECK(!WarpFromJson(nlohmann::json()));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestX25519();
    TestRegistration();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
