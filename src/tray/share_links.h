#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the user hands the tray to add a subscription - a paste, a file, the
// text of a QR code - sorted out whatever shape it came in: a sing-box
// config (JSON), https:// subscription links, and proxy share links
// (vless://, vmess://, trojan://, ss://, hysteria2:// or hy2://, tuic://) -
// one or many, on lines, between spaces, glued end to end, or the whole lot
// base64-encoded the way subscription servers send them. Share links become
// sing-box outbounds and a config around them. No Win32 here: unit-tested
// in tests/unit/share_links_test.cpp, fuzzed in tests/fuzz/tray_input_fuzz.cpp.

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
  std::optional<std::string> json;  // a JSON object: a sing-box config
  std::string jsonError;            // text that starts like JSON but isn't
  std::vector<std::string> urls;    // https:// subscription links
  std::vector<std::string> links;   // share links
  bool Empty() const { return !json && urls.empty() && links.empty(); }
};
ImportItems RecognizeImport(std::string_view text);

// One share link as a sing-box outbound (JSON text, no "tag") and the name
// it carries - or why it can't be one.
struct ParsedLink {
  std::optional<std::string> outbound;
  std::string name;  // from '#', else host:port
  std::string error;
};
ParsedLink ParseShareLink(std::string_view link);

// A whole config over `links`' servers: a TUN inbound, DNS through the
// proxy, a "proxy" selector (and "auto", a URL test, when there are several)
// that route.final uses. Links that don't convert are listed in `errors` and
// left out; no config when none converts.
struct LinksConfig {
  std::optional<std::string> config;
  std::size_t servers = 0;
  std::vector<std::string> errors;  // "ключ 2: ..." for the user
};
LinksConfig BuildConfigFromLinks(const std::vector<std::string>& links);

}  // namespace sovereign::tray
