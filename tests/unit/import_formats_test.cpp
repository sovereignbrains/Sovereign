// Import in every shape it comes in (src/tray/share_links.h,
// import_formats.h): other clients' formats (Clash/Mihomo YAML and JSON, Xray
// and v2rayN JSON, SIP008, bare sing-box outbounds), deep links, the proxy
// schemes beyond V2Ray's, percent-encoding, HTML. The keys are made up.
//
// With `--check <sing-box.exe>` every config built here is also run through
// the pinned sing-box's `check` - the configs must be ones sing-box takes,
// not just JSON that looks right (ctest: import-configs-singbox-check).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "check.h"
#include "outbounds.gen.h"
#include "share_links.h"

namespace {

using nlohmann::json;
using sovereign::tray::BuildConfig;
using sovereign::tray::ConfigFromSubscription;
using sovereign::tray::EncodeBase64;
using sovereign::tray::ImportItems;
using sovereign::tray::ParseShareLink;
using sovereign::tray::RecognizeImport;

constexpr std::string_view kUuid = "11111111-2222-3333-4444-555555555555";
constexpr std::string_view kRealityKey = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE";

// WireGuard keys: base64 of 32 bytes, as sing-box checks them.
std::string WgKey(char fill) { return EncodeBase64(std::string(32, fill)); }

std::string Percent(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_') {
      out.push_back(c);
    } else {
      out += {'%', kHex[byte >> 4], kHex[byte & 15]};
    }
  }
  return out;
}

// Configs to run through sing-box check: name, config.
std::vector<std::pair<std::string, std::string>>& Built() {
  static std::vector<std::pair<std::string, std::string>> built;
  return built;
}

json Config(const std::string& name, const ImportItems& items, std::size_t servers) {
  const auto result = BuildConfig(items);
  CHECK(result.servers == servers);
  if (result.servers != servers) {
    std::cerr << "  " << name << ": " << result.servers << " servers, errors:\n";
    for (const auto& error : result.errors) {
      std::cerr << "    " << error << "\n";
    }
  }
  if (!result.config) {
    return json::object();
  }
  Built().emplace_back(name, *result.config);
  json config = json::parse(*result.config);
  // What sing-box check doesn't look at (a selector's tags resolve only at
  // start): every tag the groups name is there, and once.
  std::vector<std::string> tags;
  for (const char* list : {"outbounds", "endpoints"}) {
    for (const json& item : config.value(list, json::array())) {
      tags.push_back(item.value("tag", ""));
    }
  }
  for (const json& group : config["outbounds"]) {
    for (const json& member : group.value("outbounds", json::array())) {
      CHECK(std::count(tags.begin(), tags.end(), member.get<std::string>()) == 1);
    }
  }
  return config;
}

// The outbound or endpoint tagged `tag`.
json Tagged(const json& config, std::string_view tag) {
  for (const char* list : {"outbounds", "endpoints"}) {
    if (config.contains(list)) {
      for (const json& item : config[list]) {
        if (item.value("tag", "") == tag) {
          return item;
        }
      }
    }
  }
  std::cerr << "  no " << tag << "\n";
  return json::object();
}

json Outbound(std::string_view link) {
  const auto parsed = ParseShareLink(link);
  if (!parsed.outbound) {
    std::cerr << "  " << link << ": " << parsed.error << "\n";
    return json::object();
  }
  return json::parse(*parsed.outbound);
}

std::string ClashYaml() {
  return std::string(
             "mixed-port: 7890\n"
             "allow-lan: false\n"
             "proxies:\n"
             "  - name: \"NL vless\"\n"
             "    type: vless\n"
             "    server: nl.example.com\n"
             "    port: 443\n"
             "    uuid: ") +
         std::string(kUuid) +
         "\n"
         "    network: tcp\n"
         "    tls: true\n"
         "    udp: true\n"
         "    flow: xtls-rprx-vision\n"
         "    servername: www.microsoft.com\n"
         "    reality-opts:\n"
         "      public-key: " +
         std::string(kRealityKey) +
         "\n"
         "      short-id: 6ba85179e30d4fc2\n"
         "    client-fingerprint: chrome\n"
         "  - {name: DE ws, type: vmess, server: de.example.com, port: 443, uuid: " +
         std::string(kUuid) +
         ", alterId: 0, cipher: auto, tls: true, servername: de.example.com, network: ws, ws-opts: {path: /ray, "
         "headers: {Host: cdn.example.com}}}\n"
         "  - name: SS obfs\n"
         "    type: ss\n"
         "    server: 1.2.3.4\n"
         "    port: 8388\n"
         "    cipher: aes-256-gcm\n"
         "    password: \"p@ss:w#rd\"\n"
         "    plugin: obfs\n"
         "    plugin-opts: {mode: http, host: bing.com}\n"
         "  - name: HY2\n"
         "    type: hysteria2\n"
         "    server: hy.example.com\n"
         "    port: 443\n"
         "    ports: 20000-30000\n"
         "    password: pw\n"
         "    obfs: salamander\n"
         "    obfs-password: ob\n"
         "    sni: hy.example.com\n"
         "    skip-cert-verify: true\n"
         "  - name: HY1\n"
         "    type: hysteria\n"
         "    server: hy1.example.com\n"
         "    port: 443\n"
         "    auth-str: secret\n"
         "    up: 30 Mbps\n"
         "    down: 100 Mbps\n"
         "    sni: hy1.example.com\n"
         "  - name: TUIC\n"
         "    type: tuic\n"
         "    server: tuic.example.com\n"
         "    port: 443\n"
         "    uuid: " +
         std::string(kUuid) +
         "\n"
         "    password: pw\n"
         "    congestion-controller: bbr\n"
         "    alpn: [h3]\n"
         "  - name: Trojan gRPC\n"
         "    type: trojan\n"
         "    server: tr.example.com\n"
         "    port: 443\n"
         "    password: pw\n"
         "    network: grpc\n"
         "    grpc-opts: {grpc-service-name: svc}\n"
         "    sni: tr.example.com\n"
         "  - name: WARP\n"
         "    type: wireguard\n"
         "    server: 162.159.192.1\n"
         "    port: 2408\n"
         "    ip: 172.16.0.2\n"
         "    ipv6: 2606:4700:110:8a36::2\n"
         "    private-key: " +
         WgKey('a') +
         "\n"
         "    public-key: " +
         WgKey('b') +
         "\n"
         "    reserved: [1, 2, 3]\n"
         "    mtu: 1280\n"
         "  - name: AnyTLS\n"
         "    type: anytls\n"
         "    server: any.example.com\n"
         "    port: 443\n"
         "    password: pw\n"
         "    sni: any.example.com\n"
         "    client-fingerprint: chrome\n"
         "  - {name: SOCKS, type: socks5, server: 10.0.0.1, port: 1080, username: u, password: p}\n"
         "  - {name: HTTPS proxy, type: http, server: 10.0.0.2, port: 3128, tls: true, sni: proxy.example.com}\n"
         "  - {name: Snell, type: snell, server: sn.example.com, port: 443, psk: key, version: 4, obfs-opts: {mode: "
         "http, host: bing.com}}\n"
         "  - {name: SSH, type: ssh, server: ssh.example.com, port: 22, username: root, password: pw}\n"
         "  - {name: Old SSR, type: ssr, server: ssr.example.com, port: 443, cipher: aes-256-cfb, password: p, "
         "obfs: plain, protocol: origin}\n"
         "  - {name: DIRECT, type: direct}\n"
         "proxy-groups:\n"
         "  - {name: PROXY, type: select, proxies: [NL vless, DE ws]}\n"
         "rules:\n"
         "  - MATCH,PROXY\n";
}

void TestClashYaml() {
  const ImportItems items = RecognizeImport(ClashYaml());
  CHECK(items.links.size() == 13);
  CHECK(items.skipped.size() == 1);  // SSR
  CHECK(!items.skipped.empty() && items.skipped[0].starts_with("Old SSR: "));
  const json config = Config("clash-yaml", items, 13);

  const json nl = Tagged(config, "NL vless");
  CHECK(nl["type"] == "vless");
  CHECK(nl["flow"] == "xtls-rprx-vision");
  CHECK(nl["tls"]["server_name"] == "www.microsoft.com");
  CHECK(nl["tls"]["reality"]["public_key"] == std::string(kRealityKey));
  CHECK(nl["tls"]["reality"]["short_id"] == "6ba85179e30d4fc2");
  CHECK(nl["tls"]["utls"]["fingerprint"] == "chrome");

  const json de = Tagged(config, "DE ws");
  CHECK(de["type"] == "vmess");
  CHECK(de["transport"]["type"] == "ws");
  CHECK(de["transport"]["path"] == "/ray");
  CHECK(de["transport"]["headers"]["Host"] == "cdn.example.com");
  CHECK(de["tls"]["server_name"] == "de.example.com");

  const json ss = Tagged(config, "SS obfs");
  CHECK(ss["password"] == "p@ss:w#rd");  // what a link would have to encode
  CHECK(ss["plugin"] == "obfs-local");
  CHECK(ss["plugin_opts"] == "obfs=http;obfs-host=bing.com");

  const json hy2 = Tagged(config, "HY2");
  CHECK(hy2["server_port"] == 443);
  CHECK(hy2["server_ports"] == "20000:30000");  // a list of one: the item, as sing-box writes it
  CHECK(hy2["obfs"]["password"] == "ob");
  CHECK(hy2["tls"]["insecure"] == true);

  const json hy1 = Tagged(config, "HY1");
  CHECK(hy1["type"] == "hysteria");
  CHECK(hy1["up_mbps"] == 30);
  CHECK(hy1["down_mbps"] == 100);
  CHECK(hy1["auth_str"] == "secret");

  CHECK(Tagged(config, "Trojan gRPC")["transport"]["service_name"] == "svc");
  CHECK(Tagged(config, "TUIC")["congestion_control"] == "bbr");
  CHECK(Tagged(config, "AnyTLS")["tls"]["utls"]["fingerprint"] == "chrome");
  CHECK(Tagged(config, "SOCKS")["username"] == "u");
  CHECK(Tagged(config, "HTTPS proxy")["tls"]["server_name"] == "proxy.example.com");
  CHECK(Tagged(config, "Snell")["obfs_mode"] == "http");
  CHECK(Tagged(config, "SSH")["user"] == "root");

  // WireGuard is an endpoint, and the selector takes it all the same.
  const json warp = Tagged(config, "WARP");
  CHECK(config.contains("endpoints") && config["endpoints"].size() == 1);
  CHECK(warp["address"] == json::array({"172.16.0.2/32", "2606:4700:110:8a36::2/128"}));
  CHECK(warp["peers"][0]["reserved"] == "AQID");  // []uint8 {1, 2, 3}: base64, as Go writes it
  CHECK(warp["peers"][0]["public_key"] == WgKey('b'));
  CHECK(warp["mtu"] == 1280);
  const json& group = config["outbounds"][1];
  CHECK(group["tag"] == "auto" && group["outbounds"].size() == 13);

  // Base64 as subscriptions send it, and the same as JSON.
  CHECK(RecognizeImport(EncodeBase64(ClashYaml())).links.size() == 13);
  const auto clashJson = RecognizeImport(
      R"({"proxies":[{"name":"J","type":"trojan","server":"a.example.com","port":443,"password":"pw"}]})");
  CHECK(clashJson.links.size() == 1 && !clashJson.json);
}

void TestClashProviders() {
  const auto items = RecognizeImport(
      "proxy-providers:\n"
      "  sub:\n"
      "    type: http\n"
      "    url: \"https://sub.example.com/clash?token=abc\"\n"
      "    interval: 3600\n"
      "  local:\n"
      "    type: file\n"
      "    path: ./x.yaml\n");
  CHECK(items.urls == std::vector<std::string>({"https://sub.example.com/clash?token=abc"}));
  CHECK(!items.Servers());

  // Not YAML after all: nothing, and no crash.
  CHECK(RecognizeImport("proxies:\n  - [unclosed\n").Empty());
  CHECK(RecognizeImport("proxies: 5\n").Empty());
}

std::string XrayConfig(std::string_view remarks, std::string_view host) {
  return std::string(R"({"remarks":")") + std::string(remarks) +
         R"(","log":{"loglevel":"warning"},"inbounds":[{"port":10808,"protocol":"socks"}],"outbounds":[)"
         R"({"tag":"proxy","protocol":"vless","settings":{"vnext":[{"address":")" +
         std::string(host) + R"(","port":443,"users":[{"id":")" + std::string(kUuid) +
         R"(","encryption":"none","flow":"xtls-rprx-vision"}]}]},)"
         R"("streamSettings":{"network":"tcp","security":"reality","realitySettings":{"serverName":"www.microsoft.com",)"
         R"("fingerprint":"firefox","publicKey":")" +
         std::string(kRealityKey) + R"(","shortId":"6ba8"}}},)"
         R"({"tag":"direct","protocol":"freedom"},{"tag":"block","protocol":"blackhole"}]})";
}

void TestXray() {
  const auto one = RecognizeImport(XrayConfig("Xray NL", "nl.example.com"));
  CHECK(!one.json);  // not taken as a sing-box config: it has outbounds too
  CHECK(one.links.size() == 1);
  const json config = Config("xray", one, 1);
  const json nl = Tagged(config, "Xray NL");
  CHECK(nl["type"] == "vless");
  CHECK(nl["tls"]["reality"]["short_id"] == "6ba8");
  CHECK(nl["tls"]["utls"]["fingerprint"] == "firefox");

  // v2rayN's subscription: a list of whole configs.
  const auto list = RecognizeImport("[" + XrayConfig("A", "a.example.com") + "," + XrayConfig("B", "b.example.com") + "]");
  CHECK(list.links.size() == 2);
  const json both = Config("xray-list", list, 2);
  CHECK(Tagged(both, "B")["server"] == "b.example.com");

  // One outbound on its own, and the protocols beside VLESS.
  const auto trojan = RecognizeImport(
      R"({"protocol":"trojan","tag":"T","settings":{"servers":[{"address":"t.example.com","port":443,"password":"pw"}]},)"
      R"("streamSettings":{"network":"ws","security":"tls","wsSettings":{"path":"/t","headers":{"Host":"h.example.com"}},)"
      R"("tlsSettings":{"serverName":"t.example.com","alpn":["h2","http/1.1"]}}})");
  const json t = Tagged(Config("xray-trojan", trojan, 1), "T");
  CHECK(t["transport"]["path"] == "/t");
  CHECK(t["transport"]["headers"]["Host"] == "h.example.com");
  CHECK(t["tls"]["alpn"] == json::array({"h2", "http/1.1"}));

  const auto others = RecognizeImport(
      R"({"outbounds":[)"
      R"({"tag":"vm","protocol":"vmess","settings":{"vnext":[{"address":"vm.example.com","port":443,"users":[{"id":")" +
      std::string(kUuid) +
      R"(","alterId":0,"security":"auto"}]}]},"streamSettings":{"network":"grpc","security":"tls","grpcSettings":{"serviceName":"g"}}},)"
      R"({"tag":"ss","protocol":"shadowsocks","settings":{"servers":[{"address":"1.2.3.4","port":8388,"method":"chacha20-ietf-poly1305","password":"p"}]}},)"
      R"({"tag":"sx","protocol":"socks","settings":{"servers":[{"address":"5.6.7.8","port":1080,"users":[{"user":"u","pass":"p"}]}]}},)"
      R"({"tag":"xh","protocol":"vless","settings":{"vnext":[{"address":"x.example.com","port":443,"users":[{"id":")" +
      std::string(kUuid) +
      R"("}]}]},"streamSettings":{"network":"xhttp","security":"tls"}}]})");
  CHECK(others.links.size() == 4);
  const auto built = BuildConfig(others);
  CHECK(built.servers == 3);  // sing-box has no xhttp
  CHECK(built.errors.size() == 1 && built.errors[0].starts_with("xh: "));
  if (built.config) {
    Built().emplace_back("xray-others", *built.config);
    const json config2 = json::parse(*built.config);
    CHECK(Tagged(config2, "vm")["transport"]["service_name"] == "g");
    CHECK(Tagged(config2, "sx")["password"] == "p");
  }
}

void TestSip008AndSingBox() {
  const auto sip = RecognizeImport(
      R"({"version":1,"servers":[{"id":"1","remarks":"S1","server":"1.1.1.1","server_port":8388,"password":"p",)"
      R"("method":"chacha20-ietf-poly1305","plugin":"obfs-local","plugin_opts":"obfs=http;obfs-host=x.com"},)"
      R"({"id":"2","remarks":"S2","server":"2.2.2.2","server_port":8389,"password":"q","method":"aes-128-gcm"}]})");
  const json config = Config("sip008", sip, 2);
  CHECK(Tagged(config, "S1")["plugin_opts"] == "obfs=http;obfs-host=x.com");
  CHECK(Tagged(config, "S2")["method"] == "aes-128-gcm");

  // Only outbounds: they get a config around them; groups are dropped.
  const auto bare = RecognizeImport(
      R"({"outbounds":[{"type":"selector","tag":"sel","outbounds":["a"]},)"
      R"({"type":"trojan","tag":"a","server":"x.example.com","server_port":443,"password":"p","tls":{"enabled":true}},)"
      R"({"type":"direct","tag":"direct"}],"endpoints":[{"type":"wireguard","tag":"w","address":["10.0.0.2/32"],)"
      R"("private_key":")" + WgKey('c') + R"(","peers":[{"address":"9.9.9.9","port":51820,"public_key":")" + WgKey('d') +
      R"(","allowed_ips":["0.0.0.0/0"]}]}]})");
  CHECK(!bare.json);
  CHECK(bare.outbounds.size() == 2);
  const json made = Config("singbox-bare", bare, 2);
  CHECK(Tagged(made, "a")["password"] == "p");
  CHECK(Tagged(made, "w")["peers"][0]["port"] == 51820);

  // One outbound, and a list of them.
  CHECK(RecognizeImport(R"({"type":"shadowsocks","tag":"one","server":"1.1.1.1","server_port":1,"method":"aes-128-gcm","password":"p"})")
            .outbounds.size() == 1);
  CHECK(RecognizeImport(R"([{"type":"socks","server":"1.1.1.1","server_port":1},{"type":"http","server":"2.2.2.2","server_port":2}])")
            .outbounds.size() == 2);

  // A whole config (it has inbounds) is taken as it is.
  const std::string whole =
      R"({"inbounds":[{"type":"mixed","listen_port":2080}],"outbounds":[{"type":"trojan","tag":"a","server":"x","server_port":1,"password":"p"}]})";
  const auto kept = RecognizeImport(whole);
  CHECK(kept.json == std::optional<std::string>(whole));
  CHECK(ConfigFromSubscription(whole).config == std::optional<std::string>(whole));
}

void TestDeepLinks() {
  const auto url = [](std::string_view text) {
    const auto items = RecognizeImport(text);
    return items.urls.empty() ? std::string() : items.urls.front();
  };
  CHECK(url("sing-box://import-remote-profile?url=" + Percent("https://sub.example.com/s?token=abc&x=1") +
            "#My%20Sub") == "https://sub.example.com/s?token=abc&x=1");
  CHECK(url("clash://install-config?url=" + Percent("https://a.example.com/clash") + "&name=A") ==
        "https://a.example.com/clash");
  CHECK(url("clash://install-config?url=https://c.example.com/a?b=1&c=2") == "https://c.example.com/a?b=1&c=2");
  CHECK(url("hiddify://import/https://h.example.com/sub/xyz#Name") == "https://h.example.com/sub/xyz");
  CHECK(url("happ://add/https://happ.example.com/s") == "https://happ.example.com/s");
  CHECK(url("sub://" + EncodeBase64("https://b64.example.com/s") + "#n") == "https://b64.example.com/s");
  CHECK(url("Открой в клиенте: v2raytun://import/" + Percent("https://t.example.com/x") + " спасибо") ==
        "https://t.example.com/x");

  // A deep link that carries a key, not a subscription.
  const auto key = RecognizeImport("v2rayng://install-config?url=" + Percent("trojan://pw@k.example.com:443#K"));
  CHECK(key.links == std::vector<std::string>({"trojan://pw@k.example.com:443#K"}));
}

void TestShapes() {
  // All of it percent-encoded.
  const auto encoded = RecognizeImport(Percent("vless://" + std::string(kUuid) + "@h.example.com:443?security=tls#N"));
  CHECK(encoded.links.size() == 1);
  // Out of a web page's source.
  const auto html = RecognizeImport(
      R"(<p><a href="trojan://pw@a.example.com:443?type=ws&amp;path=%2Fx#A">A</a></p>)");
  CHECK(html.links.size() == 1);
  const json a = Outbound(html.links.empty() ? "" : html.links[0]);
  CHECK(a["transport"]["path"] == "/x");
  CHECK(!html.links.empty() && ParseShareLink(html.links[0]).name == "A");
  // An http:// subscription is found, and the tray says it takes https:// only.
  CHECK(RecognizeImport("http://plain.example.com/sub").urls.size() == 1);
  // A subscription link's '#' is not a server name.
  CHECK(RecognizeImport("https://s.example.com/x#My Sub").urls ==
        std::vector<std::string>({"https://s.example.com/x"}));
}

void TestSchemes() {
  const json anytls = Outbound("anytls://pw@any.example.com:443?sni=any.example.com&insecure=1#A");
  CHECK(anytls["type"] == "anytls");
  CHECK(anytls["password"] == "pw");
  CHECK(anytls["tls"]["server_name"] == "any.example.com");
  CHECK(anytls["tls"]["insecure"] == true);

  const json socksBase64 = Outbound("socks://dTpw@1.2.3.4:1080#S");  // base64("u:p"), v2rayN's way
  CHECK(socksBase64["username"] == "u" && socksBase64["password"] == "p");
  const json socks6 = Outbound("socks5://u:p@[::1]:1080");
  CHECK(socks6["server"] == "::1");
  CHECK(Outbound("socks4://1.2.3.4:1080")["version"] == "4");

  const json naive = Outbound("naive+https://u:p@n.example.com#N");
  CHECK(naive["type"] == "naive");
  CHECK(naive["server_port"] == 443);
  CHECK(naive["tls"]["server_name"] == "n.example.com");
  CHECK(Outbound("naive+quic://u:p@n.example.com:8443")["quic"] == true);
  CHECK(RecognizeImport("naive+https://u:p@n.example.com#N").links.size() == 1);  // not a web link

  const json hy1 = Outbound(
      "hysteria://h.example.com:443?protocol=udp&auth=secret&peer=h.example.com&insecure=1&upmbps=30&downmbps=100"
      "&alpn=hysteria&obfsParam=xyz#H1");
  CHECK(hy1["up_mbps"] == 30 && hy1["down_mbps"] == 100);
  CHECK(hy1["auth_str"] == "secret");
  CHECK(hy1["obfs"] == "xyz");
  CHECK(hy1["tls"]["server_name"] == "h.example.com");
  CHECK(!ParseShareLink("hysteria://h.example.com:443?protocol=faketcp").outbound);

  const json wg = Outbound("wireguard://" + Percent(WgKey('a')) + "@162.159.192.1:2408?publickey=" +
                           Percent(WgKey('b')) + "&address=172.16.0.2/32,2606:4700::2&reserved=1,2,3&mtu=1280#WG");
  CHECK(wg["type"] == "wireguard");
  CHECK(wg["private_key"] == WgKey('a'));
  CHECK(wg["address"] == json::array({"172.16.0.2/32", "2606:4700::2/128"}));
  CHECK(wg["peers"][0]["reserved"] == "AQID");
  CHECK(wg["peers"][0]["allowed_ips"] == json::array({"0.0.0.0/0", "::/0"}));
  const json hiddify = Outbound("wg://162.159.192.1:2408?pk=" + Percent(WgKey('a')) + "&local_address=10.0.0.2&peer_pk=" +
                                Percent(WgKey('b')));
  CHECK(hiddify["address"] == "10.0.0.2/32");
  CHECK(!ParseShareLink("wireguard://key@1.2.3.4:51820").outbound);  // no peer key, no address

  const json ssh = Outbound("ssh://root:pw@s.example.com#SSH");
  CHECK(ssh["server_port"] == 22 && ssh["user"] == "root" && ssh["password"] == "pw");
  const json snell = Outbound("snell://key@sn.example.com:443?version=4&obfs=http&obfs-host=bing.com");
  CHECK(snell["version"] == 4 && snell["psk"] == "key" && snell["obfs_host"] == "bing.com");
  CHECK(!ParseShareLink("snell://key@sn.example.com:443?version=3").outbound);
  CHECK(ParseShareLink("ssr://abc").error.find("ShadowsocksR") != std::string::npos);

  const std::vector<std::string> links = {
      "anytls://pw@any.example.com:443?sni=any.example.com#AnyTLS",
      "socks://dTpw@1.2.3.4:1080#Socks",
      "naive+https://u:p@n.example.com#Naive",
      "naive+quic://u:p@nq.example.com:8443#NaiveQuic",
      "hysteria://h.example.com:443?auth=secret&upmbps=30&downmbps=100#Hy1",
      "wireguard://" + Percent(WgKey('a')) + "@162.159.192.1:2408?publickey=" + Percent(WgKey('b')) +
          "&address=172.16.0.2/32&reserved=1,2,3#WG",
      "ssh://root:pw@s.example.com#SSH",
      "snell://key@sn.example.com:443?version=4&obfs=http&obfs-host=bing.com#Snell",
      "vmess://" + std::string(kUuid) + "@vm.example.com:443?encryption=auto&aid=0&security=tls&type=ws&path=%2Fv#VM",
  };
  std::string pasted;
  for (const std::string& link : links) {
    pasted += link + "\n";
  }
  const auto items = RecognizeImport(pasted);
  CHECK(items.links == links);
  Config("schemes", items, links.size());
}

int RunSingBox(const std::string& singBox, const std::wstring& arguments) {
  std::wstring command = L"\"" + std::filesystem::path(singBox).wstring() + L"\" " + arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  DWORD code = 1;
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  return static_cast<int>(code);
}

namespace opt = sovereign::codegen::option;

template <typename Options>
json ReadAndWrite(const json& fields) {
  return json(fields.get<Options>());
}

// sing-box's own output read into the generated struct and written back: it
// must come out the same - the generated reading (unions too) checked against
// the real thing, not only the writing.
std::optional<json> ThroughGenerated(const json& outbound) {
  using Convert = json (*)(const json&);
  static const std::map<std::string, Convert, std::less<>> kTypes = {
      {"vless", ReadAndWrite<opt::VLESSOutboundOptions>},
      {"vmess", ReadAndWrite<opt::VMessOutboundOptions>},
      {"trojan", ReadAndWrite<opt::TrojanOutboundOptions>},
      {"shadowsocks", ReadAndWrite<opt::ShadowsocksOutboundOptions>},
      {"hysteria2", ReadAndWrite<opt::Hysteria2OutboundOptions>},
      {"hysteria", ReadAndWrite<opt::HysteriaOutboundOptions>},
      {"tuic", ReadAndWrite<opt::TUICOutboundOptions>},
      {"anytls", ReadAndWrite<opt::AnyTLSOutboundOptions>},
      {"socks", ReadAndWrite<opt::SOCKSOutboundOptions>},
      {"http", ReadAndWrite<opt::HTTPOutboundOptions>},
      {"naive", ReadAndWrite<opt::NaiveOutboundOptions>},
      {"ssh", ReadAndWrite<opt::SSHOutboundOptions>},
      {"snell", ReadAndWrite<opt::SnellOutboundOptions>},
      {"wireguard", ReadAndWrite<opt::WireGuardEndpointOptions>},
  };
  const auto it = kTypes.find(outbound.value("type", ""));
  if (it == kTypes.end()) {
    return std::nullopt;
  }
  json fields = outbound;
  fields.erase("type");
  fields.erase("tag");
  json out = it->second(fields);
  return out;
}

// Our servers as sing-box writes them: `sing-box format` reads the config
// into its own option types and writes it back. Anything it would write
// differently - a field it doesn't know, a value in another shape - is a
// difference here. The frame's selector/URL test/direct aren't from links;
// "singbox-bare" keeps the user's own outbounds as they came.
int CompareWithFormat(const std::string& name, const json& ours, const json& formatted) {
  if (name == "singbox-bare") {
    return 0;
  }
  int differences = 0;
  for (const char* key : {"outbounds", "endpoints"}) {
    if (!ours.contains(key)) {
      continue;
    }
    const json& mine = ours[key];
    const json& theirs = formatted.contains(key) ? formatted[key] : json::array();
    for (std::size_t i = 0; i < mine.size(); ++i) {
      const std::string type = mine[i].value("type", "");
      if (type == "selector" || type == "urltest" || type == "direct") {
        continue;
      }
      if (i >= theirs.size() || mine[i] != theirs[i]) {
        std::cout << "  " << name << " " << key << "[" << i << "]\n    ours:     " << mine[i].dump()
                  << "\n    sing-box: " << (i < theirs.size() ? theirs[i].dump() : "-") << "\n";
        ++differences;
        continue;
      }
      json fields = theirs[i];
      fields.erase("type");
      fields.erase("tag");
      const auto read = ThroughGenerated(theirs[i]);
      if (!read || *read != fields) {
        std::cout << "  " << name << " " << key << "[" << i << "] read back\n    sing-box:  " << fields.dump()
                  << "\n    generated: " << (read ? read->dump() : "no generated type for " + type) << "\n";
        ++differences;
      }
    }
  }
  return differences;
}

// What sing-box itself says about the configs made above.
int CheckWithSingBox(const std::string& singBox) {
  const auto dir = std::filesystem::temp_directory_path() / "sovereign-import-check";
  std::filesystem::create_directories(dir);
  int failed = 0;
  for (const auto& [name, config] : Built()) {
    const auto path = dir / (name + ".json");
    std::ofstream(path, std::ios::binary) << config;
    const int code = RunSingBox(singBox, L"check -c \"" + path.wstring() + L"\"");
    const auto formattedPath = dir / (name + ".formatted.json");
    std::ofstream(formattedPath, std::ios::binary) << config;
    int differences = -1;
    if (RunSingBox(singBox, L"format -w -c \"" + formattedPath.wstring() + L"\"") == 0) {
      std::ifstream in(formattedPath, std::ios::binary);
      const json formatted = json::parse(in, nullptr, /*allow_exceptions=*/false);
      differences = formatted.is_object() ? CompareWithFormat(name, json::parse(config), formatted) : -1;
    }
    const bool ok = code == 0 && differences == 0;
    std::cout << (ok ? "ok     " : "FAILED ") << name << (differences != 0 ? " (not as sing-box writes it)" : "")
              << "\n";
    failed += ok ? 0 : 1;
  }
  return failed;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestClashYaml();
    TestClashProviders();
    TestXray();
    TestSip008AndSingBox();
    TestDeepLinks();
    TestShapes();
    TestSchemes();
    if (argc == 3 && std::string_view(argv[1]) == "--check") {
      if (CheckWithSingBox(argv[2]) != 0) {
        return 1;
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
