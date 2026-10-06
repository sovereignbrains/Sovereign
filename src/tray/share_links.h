#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the user hands the tray to add a subscription - a paste, a file, the
// text of a QR code - sorted out whatever shape it came in:
//  - a sing-box config (JSON), or only its outbounds;
//  - https:// subscription links, bare or inside an app's deep link
//    (sing-box://import-remote-profile?url=..., clash://install-config?url=...,
//    hiddify://import/..., sub://base64 and the like);
//  - proxy share links: vless, vmess, trojan, ss, hysteria2/hy2, hysteria,
//    tuic, anytls, socks/socks5/socks4, naive+https/naive+quic, wireguard/wg,
//    ssh, snell - one or many, on lines, between spaces, glued end to end,
//    inside HTML, percent-encoded, or the whole lot base64-encoded the way
//    subscription servers send them;
//  - other clients' formats: Clash/Mihomo YAML (proxies:, proxy-providers:),
//    Xray/V2Ray JSON configs (one, or v2rayN's list of them), SIP008.
// Servers become sing-box outbounds and a config around them. No Win32 here:
// unit-tested in tests/unit/share_links_test.cpp and import_formats_test.cpp,
// fuzzed in tests/fuzz/tray_input_fuzz.cpp.

namespace sovereign::tray {

// Base64, standard or URL-safe, padded or not, whitespace ignored. nullopt
// if it isn't.
std::optional<std::string> DecodeBase64(std::string_view text);
std::string EncodeBase64(std::string_view bytes);

// Every share link in `text`, each whole, in order - however they're
// separated: whitespace, commas, nothing at all. A name after '#' may have
// spaces (a line ends it, and so does the next link).
std::vector<std::string> ExtractShareLinks(std::string_view text);

struct ImportItems {
  std::optional<std::string> json;     // a whole sing-box config, as it is
  std::string jsonError;               // text that starts like JSON but isn't
  std::vector<std::string> urls;       // http(s):// subscription links
  std::vector<std::string> links;      // share links, and servers of other formats written as ones
  std::vector<std::string> outbounds;  // sing-box outbounds and endpoints as they came (JSON, "tag" = the name)
  std::vector<std::string> skipped;    // servers sing-box can't run: "name: why"
  bool Servers() const { return !links.empty() || !outbounds.empty(); }
  bool Empty() const { return !json && urls.empty() && !Servers() && skipped.empty(); }
};
ImportItems RecognizeImport(std::string_view text);

// What an import would add, said to the user before they add it - the first
// screen offers what's on the clipboard: "подписку с packetlab.tech", "2
// подписки", "ключ VLESS", "3 ключа", "конфиг sing-box"; empty if nothing.
// Only a link's host: never its path or token.
std::string DescribeImport(const ImportItems& items);

// One share link as a sing-box outbound (JSON text, no "tag"; a WireGuard
// link gives an endpoint) and the name it carries - or why it can't be one.
struct ParsedLink {
  std::optional<std::string> outbound;
  std::string name;  // from '#', else host:port
  std::string error;
};
ParsedLink ParseShareLink(std::string_view link);

// A whole config over the servers found: a TUN inbound, DNS through the
// proxy, a "proxy" selector (and "auto", a URL test, when there are several)
// that route.final uses. Servers that don't convert are listed in `errors`
// and left out; no config when none converts.
struct LinksConfig {
  std::optional<std::string> config;
  std::size_t servers = 0;
  std::size_t found = 0;            // servers there were, converted or not
  std::vector<std::string> errors;  // "ключ 2: ..." / "NL-1: ..." for the user
};
LinksConfig BuildConfigFromLinks(const std::vector<std::string>& links);
LinksConfig BuildConfig(const ImportItems& items);

// A subscription's answer as a sing-box config: as it is when it is one,
// made from the servers it lists otherwise (keys, Clash, Xray...). No
// config when it holds neither.
LinksConfig ConfigFromSubscription(std::string_view body);

}  // namespace sovereign::tray
