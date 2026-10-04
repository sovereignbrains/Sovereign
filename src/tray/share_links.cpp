#include "share_links.h"

#include "import_formats.h"
#include "outbounds.gen.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace sovereign::tray {

namespace {

using detail::Json;

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string LowerCopy(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), Lower);
  return out;
}

bool IsAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// The proxy schemes, longest first: "vless" must win over the "ss" it ends with.
constexpr std::array<std::string_view, 17> kProxySchemes = {
    "hysteria2", "wireguard", "hysteria", "socks4a", "anytls", "socks5", "socks4", "trojan", "socks",
    "vless",     "vmess",     "snell",    "tuic",    "hy2",    "ssh",    "wg",     "ss"};

// Apps' deep links that carry a subscription link (DeepLinkUrl finds it).
constexpr std::array<std::string_view, 29> kDeepSchemes = {
    "sing-box", "clash",   "clashmeta", "clash-meta", "mihomo",   "flclash",   "stash",     "v2rayng",
    "v2raytun", "v2rayn",  "hiddify",   "karing",     "streisand", "happ",     "nekobox",   "nekoray",
    "sn",       "sub",     "husi",      "exclave",    "throne",   "shadowrocket", "surge",  "loon",
    "foxray",   "v2box",   "incy",      "npvtunnel",  "quantumult-x"};

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// %XX decoded; a '%' without two hex digits stays as it is. '+' stays too:
// it's in passwords and base64 keys far more often than it means a space.
std::string PercentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int high = HexValue(text[i + 1]);
      const int low = HexValue(text[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

std::optional<int> ParseInt(std::string_view text) {
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// The number a value starts with: "100", "100 Mbps", "100mbps".
std::optional<int> LeadingInt(std::string_view text) {
  text = Trim(text);
  std::size_t digits = 0;
  while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
    ++digits;
  }
  return ParseInt(text.substr(0, digits));
}

std::optional<int> ParsePort(std::string_view text) {
  const auto port = ParseInt(text);
  return port && *port >= 1 && *port <= 65535 ? port : std::nullopt;
}

// A share link taken apart: scheme://userinfo@host:port/path?query#name.
struct Uri {
  std::string scheme;  // lowercase
  std::string body;    // everything between "://" and '#', raw
  std::string userinfo;
  std::string host;
  std::string port;  // as written: hysteria2 allows lists and ranges
  std::string path;
  std::vector<std::pair<std::string, std::string>> query;  // keys lowercase, values decoded
  std::string name;

  // A query value, or `fallback`.
  std::string Get(std::string_view key, std::string fallback = {}) const {
    for (const auto& [k, v] : query) {
      if (k == key) {
        return v;
      }
    }
    return fallback;
  }
  // The first of `keys` that's there: clients name the same thing differently.
  std::string Any(std::initializer_list<std::string_view> keys) const {
    for (const std::string_view key : keys) {
      if (std::string value = Get(key); !value.empty()) {
        return value;
      }
    }
    return {};
  }
  bool Flag(std::string_view key) const {
    const std::string value = LowerCopy(Get(key));
    return value == "1" || value == "true" || value == "yes";
  }
};

std::optional<Uri> SplitUri(std::string_view link) {
  const std::size_t sep = link.find("://");
  if (sep == std::string_view::npos) {
    return std::nullopt;
  }
  Uri uri;
  uri.scheme = LowerCopy(link.substr(0, sep));
  std::string_view rest = link.substr(sep + 3);
  if (const std::size_t hash = rest.find('#'); hash != std::string_view::npos) {
    uri.name = std::string(Trim(PercentDecode(rest.substr(hash + 1))));
    rest = rest.substr(0, hash);
  }
  uri.body = std::string(rest);
  std::string_view queryText;
  if (const std::size_t q = rest.find('?'); q != std::string_view::npos) {
    queryText = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }
  // The userinfo ends at the last '@' (base64 in it may hold '/').
  std::string_view hostPart = rest;
  if (const std::size_t at = rest.rfind('@'); at != std::string_view::npos) {
    uri.userinfo = std::string(rest.substr(0, at));
    hostPart = rest.substr(at + 1);
  }
  if (const std::size_t slash = hostPart.find('/'); slash != std::string_view::npos) {
    uri.path = PercentDecode(hostPart.substr(slash));
    hostPart = hostPart.substr(0, slash);
  }
  if (hostPart.starts_with('[')) {
    const std::size_t close = hostPart.find(']');
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    uri.host = std::string(hostPart.substr(1, close - 1));
    hostPart = hostPart.substr(close + 1);
    if (hostPart.starts_with(':')) {
      uri.port = std::string(hostPart.substr(1));
    } else if (!hostPart.empty()) {
      return std::nullopt;
    }
  } else if (const std::size_t colon = hostPart.rfind(':'); colon != std::string_view::npos) {
    uri.host = std::string(hostPart.substr(0, colon));
    uri.port = std::string(hostPart.substr(colon + 1));
  } else {
    uri.host = std::string(hostPart);
  }
  while (!queryText.empty()) {
    const std::size_t amp = queryText.find('&');
    const std::string_view pair = queryText.substr(0, amp);
    const std::size_t eq = pair.find('=');
    if (!pair.empty()) {
      uri.query.emplace_back(LowerCopy(PercentDecode(pair.substr(0, eq))),
                             eq == std::string_view::npos ? std::string() : PercentDecode(pair.substr(eq + 1)));
    }
    if (amp == std::string_view::npos) {
      break;
    }
    queryText.remove_prefix(amp + 1);
  }
  return uri;
}

std::vector<std::string> SplitList(std::string_view text, char sep) {
  std::vector<std::string> out;
  while (!text.empty()) {
    const std::size_t at = text.find(sep);
    const std::string_view item = Trim(text.substr(0, at));
    if (!item.empty()) {
      out.emplace_back(item);
    }
    if (at == std::string_view::npos) {
      break;
    }
    text.remove_prefix(at + 1);
  }
  return out;
}

// "user:pass", percent-encoded or base64 (v2rayN's socks links): the two halves.
std::pair<std::string, std::string> Credentials(const Uri& uri) {
  std::string raw = PercentDecode(uri.userinfo);
  if (!raw.empty() && raw.find(':') == std::string::npos) {
    if (const auto decoded = DecodeBase64(raw); decoded && decoded->find(':') != std::string::npos) {
      raw = *decoded;
    }
  }
  const std::size_t colon = raw.find(':');
  if (colon == std::string::npos) {
    return {raw, {}};
  }
  return {raw.substr(0, colon), raw.substr(colon + 1)};
}

namespace opt = sovereign::codegen::option;

// What a link becomes: sing-box's own option struct for its type, generated
// from sing-box's sources (src/generated/outbounds.gen.h) - a field sing-box
// doesn't have doesn't compile, and every value is written the way sing-box
// writes it (a list of one as the item, an empty value not at all).
template <typename Options>
Json Written(std::string_view type, const Options& options) {
  Json out = Json::object();
  out["type"] = type;
  const nlohmann::json fields = options;
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    out[it.key()] = Json(it.value());
  }
  return out;
}

// The TLS block the V2Ray-style parameters describe; none when there's none.
std::optional<opt::OutboundTLSOptions> Tls(const std::string& security, const Uri& uri, const std::string& sni,
                                           const std::string& alpn, const std::string& fingerprint, bool insecure) {
  if (security != "tls" && security != "reality" && security != "xtls") {
    return std::nullopt;
  }
  opt::OutboundTLSOptions tls;
  tls.enabled = true;
  tls.serverName = sni;
  tls.insecure = insecure;
  tls.alpn = SplitList(alpn, ',');
  std::string fp = fingerprint;
  if (security == "reality") {
    if (fp.empty()) {
      fp = "chrome";  // REALITY runs over uTLS in sing-box
    }
    opt::OutboundRealityOptions reality;
    reality.enabled = true;
    reality.publicKey = uri.Get("pbk");
    reality.shortID = uri.Get("sid");
    tls.reality = std::move(reality);
  }
  if (!fp.empty() && fp != "none") {
    opt::OutboundUTLSOptions utls;
    utls.enabled = true;
    utls.fingerprint = fp;
    tls.utls = std::move(utls);
  }
  return tls;
}

bool Insecure(const Uri& uri) {
  return uri.Flag("allowinsecure") || uri.Flag("insecure") || uri.Flag("allow_insecure") ||
         uri.Flag("skip-cert-verify");
}

// The transport for a V2Ray `type`/`net`; an error for the ones sing-box
// doesn't have.
std::pair<std::optional<opt::V2RayTransportOptions>, std::string> Transport(std::string type, const std::string& host,
                                                                           std::string path,
                                                                           const std::string& serviceName,
                                                                           const std::string& headerType) {
  type = LowerCopy(type);
  if (type.empty() || type == "tcp" || type == "raw") {
    if (LowerCopy(headerType) != "http") {
      return {std::nullopt, {}};
    }
    type = "http";  // TCP with an HTTP header: sing-box's HTTP transport
  }
  opt::V2RayTransportOptions transport;
  if (type == "ws" || type == "websocket") {
    transport.type = "ws";
    // "/path?ed=2048": early data, which sing-box takes as options.
    if (const std::size_t q = path.find("?ed="); q != std::string::npos) {
      const auto early = ParseInt(std::string_view(path).substr(q + 4));
      path.resize(q);
      if (early && *early > 0) {
        transport.websocketOptions.maxEarlyData = static_cast<std::uint32_t>(*early);
        transport.websocketOptions.earlyDataHeaderName = "Sec-WebSocket-Protocol";
      }
    }
    transport.websocketOptions.path = path.empty() ? "/" : path;
    if (!host.empty()) {
      transport.websocketOptions.headers["Host"] = std::vector<std::string>{host};
    }
    return {transport, {}};
  }
  if (type == "grpc" || type == "gun") {
    transport.type = "grpc";
    transport.grpcOptions.serviceName = serviceName;
    return {transport, {}};
  }
  if (type == "http" || type == "h2") {
    transport.type = "http";
    transport.httpOptions.host = SplitList(host, ',');
    transport.httpOptions.path = path;
    return {transport, {}};
  }
  if (type == "httpupgrade") {
    transport.type = "httpupgrade";
    transport.httpUpgradeOptions.host = host;
    transport.httpUpgradeOptions.path = path.empty() ? "/" : path;
    return {transport, {}};
  }
  if (type == "quic") {
    transport.type = "quic";
    return {transport, {}};
  }
  return {std::nullopt, "транспорт " + type + " sing-box не поддерживает"};
}

template <typename Options>
bool SetServer(Options& out, const Uri& uri, std::string& error, int defaultPort = 0) {
  auto port = ParsePort(uri.port);
  if (!port && uri.port.empty() && defaultPort != 0) {
    port = defaultPort;
  }
  if (uri.host.empty() || !port) {
    error = "нет адреса или порта сервера";
    return false;
  }
  out.server = uri.host;
  out.serverPort = static_cast<std::uint16_t>(*port);
  return true;
}

template <typename Options>
void AddTransportAndTls(Options& out, const Uri& uri, const std::string& defaultSecurity, std::string& error) {
  const std::string security = LowerCopy(uri.Get("security", defaultSecurity));
  out.tls = Tls(security, uri, uri.Get("sni", uri.Get("peer")), uri.Get("alpn"), uri.Get("fp"), Insecure(uri));
  auto [transport, transportError] =
      Transport(uri.Get("type", "tcp"), uri.Get("host"), uri.Get("path"), uri.Get("servicename"), uri.Get("headertype"));
  if (!transportError.empty()) {
    error = std::move(transportError);
    return;
  }
  out.transport = std::move(transport);
}

std::string Vless(const Uri& uri, Json& out) {
  opt::VLESSOutboundOptions vless;
  std::string error;
  if (!SetServer(vless, uri, error)) {
    return error;
  }
  vless.uuid = PercentDecode(uri.userinfo);
  vless.flow = uri.Get("flow");
  AddTransportAndTls(vless, uri, "none", error);
  vless.packetEncoding = "xudp";
  if (vless.uuid.empty()) {
    return "нет UUID";
  }
  out = Written("vless", vless);
  return error;
}

std::string Trojan(const Uri& uri, Json& out) {
  opt::TrojanOutboundOptions trojan;
  std::string error;
  if (!SetServer(trojan, uri, error)) {
    return error;
  }
  trojan.password = PercentDecode(uri.userinfo);
  AddTransportAndTls(trojan, uri, "tls", error);
  out = Written("trojan", trojan);
  return error;
}

// vmess://base64(JSON) - V2RayN's format; some clients write it like vless.
std::string Vmess(const Uri& uri, Json& out) {
  const auto decoded = DecodeBase64(uri.body);
  const auto json = decoded ? nlohmann::json::parse(*decoded, nullptr, false) : nlohmann::json();
  opt::VMessOutboundOptions vmess;
  if (!json.is_object()) {
    std::string error;
    if (!SetServer(vmess, uri, error)) {
      return error;
    }
    vmess.uuid = PercentDecode(uri.userinfo);
    vmess.security = uri.Get("encryption", "auto");
    if (const auto alterId = ParseInt(uri.Get("aid")); alterId && *alterId > 0) {
      vmess.alterId = *alterId;
    }
    AddTransportAndTls(vmess, uri, "none", error);
    if (error.empty() && vmess.uuid.empty()) {
      return "нет UUID";
    }
    out = Written("vmess", vmess);
    return error;
  }
  const auto str = [&](const char* key) -> std::string {
    const auto it = json.find(key);
    if (it == json.end()) {
      return {};
    }
    if (it->is_string()) {
      return it->get<std::string>();
    }
    if (it->is_number_integer()) {
      return std::to_string(it->get<std::int64_t>());
    }
    return {};
  };
  Uri server;
  server.host = str("add");
  server.port = str("port");
  std::string error;
  if (!SetServer(vmess, server, error)) {
    return error;
  }
  vmess.uuid = str("id");
  const std::string scy = str("scy");
  vmess.security = scy.empty() ? "auto" : scy;
  if (const auto alterId = ParseInt(str("aid")); alterId && *alterId > 0) {
    vmess.alterId = *alterId;
  }
  const std::string net = str("net");
  vmess.tls = Tls(LowerCopy(str("tls")), server, str("sni"), str("alpn"), str("fp"), false);
  auto [transport, transportError] =
      Transport(net, str("host"), str("path"), LowerCopy(net) == "grpc" ? str("path") : std::string(), str("type"));
  if (!transportError.empty()) {
    return transportError;
  }
  vmess.transport = std::move(transport);
  if (vmess.uuid.empty()) {
    return "нет UUID";
  }
  out = Written("vmess", vmess);
  if (const std::string ps = str("ps"); !ps.empty()) {
    out["tag"] = ps;  // the name - taken out again by the caller
  }
  return {};
}

// SIP002: ss://base64(method:password)@host:port or ss://method:password@...
// (percent-encoded), or the old ss://base64(method:password@host:port).
std::string Shadowsocks(const Uri& uri, Json& out) {
  Uri server = uri;
  std::string credentials;
  if (uri.userinfo.empty()) {
    std::string_view body = uri.body;
    body = body.substr(0, body.find('?'));
    if (body.ends_with('/')) {
      body.remove_suffix(1);
    }
    const auto decoded = DecodeBase64(PercentDecode(body));
    if (!decoded) {
      return "не разобрать ss-ссылку";
    }
    const auto inner = SplitUri("ss://" + *decoded);
    if (!inner || inner->userinfo.empty()) {
      return "не разобрать ss-ссылку";
    }
    server.host = inner->host;
    server.port = inner->port;
    credentials = inner->userinfo;
  } else {
    const std::string raw = PercentDecode(uri.userinfo);
    const auto decoded = raw.find(':') == std::string::npos ? DecodeBase64(raw) : std::nullopt;
    credentials = decoded ? *decoded : raw;
  }
  const std::size_t colon = credentials.find(':');
  if (colon == std::string::npos || colon == 0) {
    return "нет метода шифрования или пароля";
  }
  opt::ShadowsocksOutboundOptions ss;
  std::string error;
  if (!SetServer(ss, server, error)) {
    return error;
  }
  ss.method = LowerCopy(credentials.substr(0, colon));
  ss.password = credentials.substr(colon + 1);
  if (const std::string plugin = uri.Get("plugin"); !plugin.empty()) {
    const std::size_t semi = plugin.find(';');
    std::string name = plugin.substr(0, semi);
    if (name == "simple-obfs" || name == "obfs") {
      name = "obfs-local";
    }
    if (name != "obfs-local" && name != "v2ray-plugin") {
      return "плагин " + name + " sing-box не поддерживает";
    }
    ss.plugin = name;
    if (semi != std::string::npos) {
      ss.pluginOptions = plugin.substr(semi + 1);
    }
  }
  out = Written("shadowsocks", ss);
  return {};
}

// "443", "443,20000-30000" - sing-box's server_ports are "a:b" ranges.
std::vector<std::string> PortRanges(std::string_view text) {
  std::vector<std::string> ranges;
  for (const std::string& part : SplitList(text, ',')) {
    const std::size_t dash = part.find_first_of("-:");
    const auto first = ParsePort(std::string_view(part).substr(0, dash));
    const auto last = dash == std::string::npos ? first : ParsePort(std::string_view(part).substr(dash + 1));
    if (!first || !last || *last < *first) {
      return {};
    }
    ranges.push_back(std::format("{}:{}", *first, *last));
  }
  return ranges;
}

// server_port, or server_ports for port hopping ("443,20000-30000" in the
// port or in mport).
template <typename Options>
std::string SetHoppingServer(Options& out, const Uri& uri) {
  if (uri.host.empty()) {
    return "нет адреса сервера";
  }
  out.server = uri.host;
  const std::string ports = uri.Get("mport");
  const auto single = ParsePort(uri.port);
  if (single) {
    out.serverPort = static_cast<std::uint16_t>(*single);
  }
  if (!single || !ports.empty()) {
    const auto ranges = PortRanges(ports.empty() ? uri.port : ports);
    if (ranges.empty()) {
      return "нет порта сервера";
    }
    out.serverPorts = ranges;
  }
  return {};
}

// The TLS of the QUIC protocols: always on.
opt::OutboundTLSOptions QuicTls(const Uri& uri, std::vector<std::string> defaultAlpn) {
  opt::OutboundTLSOptions tls;
  tls.enabled = true;
  tls.serverName = uri.Any({"sni", "peer"});
  tls.insecure = Insecure(uri);
  auto alpn = SplitList(uri.Get("alpn"), ',');
  tls.alpn = alpn.empty() ? std::move(defaultAlpn) : std::move(alpn);
  return tls;
}

std::string Hysteria2(const Uri& uri, Json& out) {
  opt::Hysteria2OutboundOptions hy2;
  if (std::string error = SetHoppingServer(hy2, uri); !error.empty()) {
    return error;
  }
  hy2.password = PercentDecode(uri.userinfo);
  if (const std::string obfs = uri.Get("obfs"); !obfs.empty() && obfs != "none") {
    opt::Hysteria2Obfs o;
    o.type = obfs;
    o.password = uri.Get("obfs-password");
    hy2.obfs = std::move(o);
  }
  hy2.tls = QuicTls(uri, {});
  out = Written("hysteria2", hy2);
  return {};
}

// Hysteria 1: hysteria://host:port?auth=...&upmbps=...&downmbps=...&obfsParam=...
std::string Hysteria(const Uri& uri, Json& out) {
  opt::HysteriaOutboundOptions hy;
  if (std::string error = SetHoppingServer(hy, uri); !error.empty()) {
    return error;
  }
  if (const std::string protocol = LowerCopy(uri.Get("protocol")); !protocol.empty() && protocol != "udp") {
    return "протокол " + protocol + " sing-box не поддерживает";
  }
  // The bandwidth is how fast Hysteria sends: it can't be left out. The
  // defaults are modest - too high a rate loses packets.
  hy.upMbps = LeadingInt(uri.Any({"upmbps", "up"})).value_or(10);
  hy.downMbps = LeadingInt(uri.Any({"downmbps", "down"})).value_or(50);
  hy.authString = uri.Any({"auth", "auth_str", "auth-str"});
  if (hy.authString.empty()) {
    hy.authString = PercentDecode(uri.userinfo);
  }
  if (const std::string obfs = uri.Any({"obfsparam", "obfs-password", "obfs"}); obfs != "xplus") {
    hy.obfs = obfs;
  }
  hy.tls = QuicTls(uri, {"hysteria"});
  out = Written("hysteria", hy);
  return {};
}

std::string Tuic(const Uri& uri, Json& out) {
  opt::TUICOutboundOptions tuic;
  std::string error;
  if (!SetServer(tuic, uri, error)) {
    return error;
  }
  const std::string credentials = PercentDecode(uri.userinfo);
  const std::size_t colon = credentials.find(':');
  tuic.uuid = credentials.substr(0, colon);
  if (colon != std::string::npos) {
    tuic.password = credentials.substr(colon + 1);
  }
  tuic.congestionControl = uri.Any({"congestion_control", "congestion-control", "congestion-controller"});
  tuic.udpRelayMode = uri.Any({"udp_relay_mode", "udp-relay-mode"});
  tuic.tls = QuicTls(uri, {"h3"});
  tuic.tls->disableSNI = uri.Flag("disable_sni") || uri.Flag("disable-sni");
  if (colon == 0 || credentials.empty()) {
    return "нет UUID";
  }
  out = Written("tuic", tuic);
  return {};
}

std::string AnyTls(const Uri& uri, Json& out) {
  opt::AnyTLSOutboundOptions anytls;
  std::string error;
  if (!SetServer(anytls, uri, error)) {
    return error;
  }
  anytls.password = PercentDecode(uri.userinfo);
  anytls.tls = Tls(LowerCopy(uri.Get("security", "tls")), uri, uri.Any({"sni", "peer"}), uri.Get("alpn"),
                   uri.Get("fp"), Insecure(uri));
  if (!anytls.tls) {
    return "AnyTLS без TLS не работает";
  }
  if (anytls.password.empty()) {
    return "нет пароля";
  }
  out = Written("anytls", anytls);
  return {};
}

// socks://, socks5://, socks4://, socks4a://; the credentials plain or base64.
std::string Socks(const Uri& uri, Json& out) {
  opt::SOCKSOutboundOptions socks;
  std::string error;
  if (!SetServer(socks, uri, error, 1080)) {
    return error;
  }
  if (uri.scheme == "socks4" || uri.scheme == "socks4a") {
    socks.version = uri.scheme.substr(5);
  }
  std::tie(socks.username, socks.password) = Credentials(uri);
  out = Written("socks", socks);
  return {};
}

// An HTTP proxy (from Clash and Xray; a pasted http(s):// link is a subscription).
std::string HttpProxy(const Uri& uri, Json& out) {
  opt::HTTPOutboundOptions http;
  std::string error;
  if (!SetServer(http, uri, error, uri.scheme == "https" ? 443 : 80)) {
    return error;
  }
  std::tie(http.username, http.password) = Credentials(uri);
  if (uri.scheme == "https") {
    http.tls = Tls("tls", uri, uri.Any({"sni", "peer"}), uri.Get("alpn"), uri.Get("fp"), Insecure(uri));
  }
  out = Written("http", http);
  return {};
}

// naive+https://user:pass@host:port, naive+quic://... - always TLS, the
// host's name for SNI.
std::string Naive(const Uri& uri, Json& out) {
  opt::NaiveOutboundOptions naive;
  std::string error;
  if (!SetServer(naive, uri, error, 443)) {
    return error;
  }
  std::tie(naive.username, naive.password) = Credentials(uri);
  naive.quic = uri.scheme == "naive+quic";
  opt::OutboundTLSOptions tls;
  tls.enabled = true;
  tls.serverName = uri.Any({"sni", "peer"}).empty() ? uri.host : uri.Any({"sni", "peer"});
  naive.tls = std::move(tls);
  out = Written("naive", naive);
  return {};
}

// "10.0.0.2/32,fd00::2" - addresses with their prefix (a bare one is a host).
std::vector<std::string> Prefixes(std::string_view text) {
  std::vector<std::string> out;
  for (std::string item : SplitList(text, ',')) {
    if (item.starts_with('[') && item.ends_with(']')) {
      item = item.substr(1, item.size() - 2);
    }
    if (item.find('/') == std::string::npos) {
      item += item.find(':') != std::string::npos ? "/128" : "/32";
    }
    out.push_back(std::move(item));
  }
  return out;
}

// "1,2,3" or base64 of three bytes: WireGuard's reserved bytes (Cloudflare WARP).
std::optional<std::string> Reserved(std::string_view text) {
  std::string bytes;
  if (text.find(',') != std::string_view::npos) {
    for (const std::string& part : SplitList(text, ',')) {
      const auto value = ParseInt(part);
      if (!value || *value < 0 || *value > 255) {
        return std::nullopt;
      }
      bytes.push_back(static_cast<char>(*value));
    }
  } else if (const auto decoded = DecodeBase64(text)) {
    bytes = *decoded;
  }
  if (bytes.size() != 3) {
    return std::nullopt;
  }
  return bytes;
}

// wireguard://privatekey@host:port?publickey=...&address=... (v2rayN), or
// wg:// with Hiddify's names. A sing-box endpoint, not an outbound.
std::string WireGuard(const Uri& uri, Json& out) {
  const auto port = ParsePort(uri.port);
  if (uri.host.empty() || !port) {
    return "нет адреса или порта сервера";
  }
  opt::WireGuardEndpointOptions wg;
  wg.privateKey = PercentDecode(uri.userinfo);
  if (wg.privateKey.empty()) {
    wg.privateKey = uri.Any({"privatekey", "private_key", "pk", "secretkey"});
  }
  opt::WireGuardPeer peer;
  peer.publicKey = uri.Any({"publickey", "public_key", "peer_pk", "peer_public_key", "pbk"});
  wg.address = Prefixes(uri.Any({"address", "ip", "local_address", "localaddress"}));
  if (wg.privateKey.empty() || peer.publicKey.empty()) {
    return "нет ключей WireGuard";
  }
  if (wg.address.values.empty()) {
    return "нет адреса интерфейса WireGuard";
  }
  if (const auto mtu = ParseInt(uri.Get("mtu")); mtu && *mtu >= 576 && *mtu <= 65535) {
    wg.mtu = static_cast<std::uint32_t>(*mtu);
  }
  peer.address = uri.host;
  peer.port = static_cast<std::uint16_t>(*port);
  peer.preSharedKey = uri.Any({"presharedkey", "pre_shared_key", "psk", "preshared_key"});
  auto allowed = Prefixes(uri.Any({"allowedips", "allowed_ips"}));
  peer.allowedIPs = allowed.empty() ? std::vector<std::string>{"0.0.0.0/0", "::/0"} : std::move(allowed);
  if (const std::string reserved = uri.Get("reserved"); !reserved.empty()) {
    const auto bytes = Reserved(reserved);
    if (!bytes) {
      return "не разобрать reserved";
    }
    peer.reserved = EncodeBase64(*bytes);  // []uint8: base64 in Go's JSON
  }
  if (const auto keepalive = ParseInt(uri.Any({"keepalive", "persistent_keepalive_interval"}));
      keepalive && *keepalive > 0 && *keepalive <= 65535) {
    peer.persistentKeepaliveInterval = static_cast<std::uint16_t>(*keepalive);
  }
  wg.peers.push_back(std::move(peer));
  out = Written("wireguard", wg);
  return {};
}

std::string Ssh(const Uri& uri, Json& out) {
  opt::SSHOutboundOptions ssh;
  std::string error;
  if (!SetServer(ssh, uri, error, 22)) {
    return error;
  }
  const auto [user, password] = Credentials(uri);
  ssh.user = user.empty() ? "root" : user;
  ssh.password = password;
  if (const std::string key = uri.Any({"pk", "private_key", "privatekey"}); !key.empty()) {
    ssh.privateKey = std::vector<std::string>{key};
  }
  ssh.hostKey = SplitList(uri.Any({"hk", "host_key", "hostkey"}), ',');
  if (password.empty() && ssh.privateKey.values.empty()) {
    return "нет пароля или ключа SSH";
  }
  out = Written("ssh", ssh);
  return {};
}

// snell://psk@host:port?version=4&obfs=http&obfs-host=... (Clash's fields);
// sing-box has versions 4 and 6.
std::string Snell(const Uri& uri, Json& out) {
  opt::SnellOutboundOptions snell;
  std::string error;
  if (!SetServer(snell, uri, error)) {
    return error;
  }
  const int version = ParseInt(uri.Get("version")).value_or(4);
  if (version != 4 && version != 6) {
    return std::format("Snell v{} sing-box не поддерживает (только 4 и 6)", version);
  }
  snell.version = version;
  snell.psk = PercentDecode(uri.userinfo);
  if (snell.psk.empty()) {
    snell.psk = uri.Get("psk");
  }
  if (snell.psk.empty()) {
    return "нет PSK";
  }
  if (version == 4) {
    if (const std::string obfs = LowerCopy(uri.Get("obfs")); !obfs.empty() && obfs != "none") {
      snell.obfsOptions.obfsMode = obfs;
      snell.obfsOptions.obfsHost = uri.Get("obfs-host");
    }
  } else {
    snell.v6Options.mode = uri.Get("mode");
  }
  out = Written("snell", snell);
  return {};
}

enum class LinkKind : std::uint8_t { Proxy, Web, Deep };

struct Found {
  std::size_t start = 0;
  LinkKind kind = LinkKind::Proxy;
};

// Where a link starts that has "://" at `sep`, and what it is - or nothing.
std::optional<Found> StartAt(std::string_view text, std::size_t sep) {
  // The whole word before "://", with the characters app names use.
  std::size_t word = sep;
  while (word > 0 && (IsAlnum(text[word - 1]) || text[word - 1] == '-' || text[word - 1] == '.' ||
                      text[word - 1] == '+' || text[word - 1] == '_')) {
    --word;
  }
  const std::string whole = LowerCopy(text.substr(word, sep - word));
  if (std::find(kDeepSchemes.begin(), kDeepSchemes.end(), whole) != kDeepSchemes.end()) {
    return Found{word, LinkKind::Deep};
  }
  if (whole.ends_with("naive+https") || whole.ends_with("naive+quic")) {
    return Found{sep - (whole.ends_with("naive+https") ? 11 : 10), LinkKind::Proxy};
  }
  std::size_t begin = sep;
  while (begin > 0 && IsAlnum(text[begin - 1])) {
    --begin;
  }
  const std::string run = LowerCopy(text.substr(begin, sep - begin));
  for (const std::string_view scheme : kProxySchemes) {
    if (run.ends_with(scheme)) {
      return Found{sep - scheme.size(), LinkKind::Proxy};
    }
  }
  // A web link starts its own word: inside a key or a deep link it's a
  // parameter's value ("...&spx=https://...", "import/https://...").
  if ((run == "https" || run == "http") &&
      (begin == 0 || IsSpace(text[begin - 1]) ||
       std::string_view("\"'<>(),;[]").find(text[begin - 1]) != std::string_view::npos)) {
    return Found{begin, LinkKind::Web};
  }
  return std::nullopt;
}

// Every link start in `text`: proxy links, web links, deep links.
std::vector<Found> FindStarts(std::string_view text) {
  std::vector<Found> starts;
  std::size_t from = 0;
  while (true) {
    const std::size_t sep = text.find("://", from);
    if (sep == std::string_view::npos) {
      break;
    }
    if (const auto found = StartAt(text, sep); found && (starts.empty() || found->start >= starts.back().start)) {
      starts.push_back(*found);
    }
    from = sep + 3;
  }
  return starts;
}

// One link's text: from its start to the next link's; whitespace ends it
// before the name, a line (or HTML around it) ends the name. A web link
// has no name.
std::string LinkAt(std::string_view text, const Found& found, std::size_t end) {
  std::string_view link = text.substr(found.start, end - found.start);
  if (found.kind == LinkKind::Web) {
    link = link.substr(0, link.find('#'));  // a fragment never reaches the server
  }
  const std::size_t hash = found.kind == LinkKind::Web ? std::string_view::npos : link.find('#');
  const std::size_t space = link.find_first_of(" \t\r\n\f\v\"'<>");
  if (space != std::string_view::npos && (hash == std::string_view::npos || space < hash)) {
    link = link.substr(0, space);
  } else if (hash != std::string_view::npos) {
    link = link.substr(0, link.find_first_of("\r\n\"<>", hash));
  }
  // A list's separator glued to the next key: "vless://...,vless://...".
  link = Trim(link);
  while (!link.empty() && (link.back() == ',' || link.back() == ';')) {
    link.remove_suffix(1);
  }
  // Copied out of a web page's source.
  std::string out(link);
  for (std::size_t at = out.find("&amp;"); at != std::string::npos; at = out.find("&amp;", at + 1)) {
    out.erase(at + 1, 4);
  }
  return out;
}

bool StartsWithHttp(std::string_view text) {
  const std::string lower = LowerCopy(text.substr(0, 8));
  return lower.starts_with("https://") || lower.starts_with("http://");
}

// The http(s) link in `text`, up to its end: whitespace, a name ('#'), a quote.
std::optional<std::string> FindHttp(std::string_view text) {
  const std::string lower = LowerCopy(text);
  std::size_t at = lower.find("https://");
  if (const std::size_t plain = lower.find("http://"); plain < at) {
    at = plain;
  }
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const std::string_view rest = text.substr(at);
  return std::string(rest.substr(0, rest.find_first_of(" \t\r\n\f\v#\"'<>")));
}

// The subscription a deep link imports: its url= (or link=, config=...)
// parameter, a link in its path (hiddify://import/https://...), or base64
// of one (sub://...).
std::optional<std::string> DeepLinkUrl(std::string_view link) {
  const std::size_t sep = link.find("://");
  const std::string_view body = link.substr(sep + 3);
  const std::string lower = LowerCopy(body);
  for (const std::string_view key : {"url=", "link=", "sub=", "config=", "remote=", "profile="}) {
    const std::size_t at = lower.find(key);
    if (at == std::string::npos || (at > 0 && body[at - 1] != '?' && body[at - 1] != '&')) {
      continue;
    }
    std::string_view value = body.substr(at + key.size());
    // Unencoded, it runs to the end (its own '&'s are its own); encoded, to the next '&'.
    if (!StartsWithHttp(value)) {
      value = value.substr(0, value.find_first_of("&#"));
    }
    std::string decoded = PercentDecode(value);
    if (!StartsWithHttp(decoded)) {
      decoded = PercentDecode(decoded);  // encoded twice
    }
    if (StartsWithHttp(decoded)) {
      return FindHttp(decoded);
    }
  }
  std::string decoded = PercentDecode(body.substr(0, body.find('#')));
  for (int pass = 0; pass < 2; ++pass) {
    if (auto url = FindHttp(decoded)) {
      return url;
    }
    decoded = PercentDecode(decoded);
  }
  if (const auto base64 = DecodeBase64(PercentDecode(body.substr(0, body.find_first_of("#?"))))) {
    return FindHttp(*base64);
  }
  return std::nullopt;
}

void AddUnique(std::vector<std::string>& into, std::string item) {
  if (std::find(into.begin(), into.end(), item) == into.end()) {
    into.push_back(std::move(item));
  }
}

void Collect(std::string_view text, ImportItems& items, int depth) {
  const auto starts = FindStarts(text);
  for (std::size_t i = 0; i < starts.size(); ++i) {
    const std::size_t end = i + 1 < starts.size() ? starts[i + 1].start : text.size();
    std::string link = LinkAt(text, starts[i], end);
    if (link.find("://") + 3 >= link.size()) {
      continue;  // a scheme and nothing after it
    }
    switch (starts[i].kind) {
      case LinkKind::Proxy:
        AddUnique(items.links, std::move(link));
        break;
      case LinkKind::Web:
        AddUnique(items.urls, std::move(link));
        break;
      case LinkKind::Deep:
        if (auto url = DeepLinkUrl(link)) {
          AddUnique(items.urls, std::move(*url));
        } else if (depth < 2) {
          // A deep link that imports a key rather than a subscription
          // (v2rayng://install-config?url=vless%3A%2F%2F...).
          Collect(PercentDecode(std::string_view(link).substr(link.find("://") + 3)), items, depth + 1);
        }
        break;
    }
  }
}

bool ContainsNoCase(std::string_view text, std::string_view needle) {
  return LowerCopy(text).find(needle) != std::string::npos;
}

void Recognize(std::string_view text, ImportItems& items, int depth) {
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3);  // a BOM from Notepad
  }
  text = Trim(text);
  if (text.starts_with('{') || text.starts_with('[')) {
    try {
      const auto json = Json::parse(text);
      if (detail::ImportJson(json, items)) {
        return;
      }
      if (json.is_object()) {
        items.json = std::string(text);
        return;
      }
    } catch (const nlohmann::json::parse_error& e) {
      // Its message says where: "at line 3, column 7".
      if (text.starts_with('{')) {
        items.jsonError = e.what();
      }
    }
  }
  if (detail::LooksLikeClashYaml(text) && detail::ImportYaml(text, items)) {
    return;
  }
  Collect(text, items, 0);
  if (!items.Empty() || depth > 0) {
    return;
  }
  // Nothing readable: maybe the whole thing is base64, as subscriptions
  // send it, or percent-encoded.
  if (const auto decoded = DecodeBase64(text)) {
    Recognize(*decoded, items, depth + 1);
  } else if (ContainsNoCase(text, "%3a%2f%2f")) {
    Recognize(PercentDecode(text), items, depth + 1);
  }
}

// The outbound's name for messages: its '#', else its place.
std::string LinkLabel(const std::string& link, std::size_t index) {
  if (const auto uri = SplitUri(link); uri && !uri->name.empty()) {
    return uri->name;
  }
  return std::format("ключ {}", index + 1);
}

}  // namespace

std::optional<std::string> DecodeBase64(std::string_view text) {
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  bool padding = false;
  std::size_t symbols = 0;
  for (const char c : text) {
    int value = -1;
    if (c >= 'A' && c <= 'Z') {
      value = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
      value = c - 'a' + 26;
    } else if (c >= '0' && c <= '9') {
      value = c - '0' + 52;
    } else if (c == '+' || c == '-') {
      value = 62;
    } else if (c == '/' || c == '_') {
      value = 63;
    } else if (c == '=') {
      padding = true;
      continue;
    } else if (IsSpace(c)) {
      continue;
    } else {
      return std::nullopt;
    }
    if (padding) {
      return std::nullopt;  // data after padding
    }
    ++symbols;
    buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }
  if (symbols == 0 || symbols % 4 == 1) {
    return std::nullopt;
  }
  return out;
}

std::string EncodeBase64(std::string_view bytes) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const std::uint32_t n = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                            (static_cast<std::uint8_t>(bytes[i + 1]) << 8) | static_cast<std::uint8_t>(bytes[i + 2]);
    out += {kAlphabet[(n >> 18) & 63], kAlphabet[(n >> 12) & 63], kAlphabet[(n >> 6) & 63], kAlphabet[n & 63]};
  }
  if (const std::size_t left = bytes.size() - i; left > 0) {
    std::uint32_t n = static_cast<std::uint8_t>(bytes[i]) << 16;
    if (left == 2) {
      n |= static_cast<std::uint8_t>(bytes[i + 1]) << 8;
    }
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += left == 2 ? kAlphabet[(n >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

std::vector<std::string> ExtractShareLinks(std::string_view text) {
  ImportItems items;
  Collect(text, items, 0);
  if (items.links.empty()) {
    if (const auto decoded = DecodeBase64(Trim(text))) {
      Collect(*decoded, items, 1);
    }
  }
  return items.links;
}

ImportItems RecognizeImport(std::string_view text) {
  ImportItems items;
  Recognize(text, items, 0);
  return items;
}

ParsedLink ParseShareLink(std::string_view link) {
  ParsedLink parsed;
  const auto uri = SplitUri(link);
  if (!uri) {
    parsed.error = "не ссылка";
    return parsed;
  }
  Json out = Json::object();
  std::string error;
  const std::string& scheme = uri->scheme;
  if (scheme == "vless") {
    error = Vless(*uri, out);
  } else if (scheme == "vmess") {
    error = Vmess(*uri, out);
  } else if (scheme == "trojan") {
    error = Trojan(*uri, out);
  } else if (scheme == "ss") {
    error = Shadowsocks(*uri, out);
  } else if (scheme == "hysteria2" || scheme == "hy2") {
    error = Hysteria2(*uri, out);
  } else if (scheme == "hysteria") {
    error = Hysteria(*uri, out);
  } else if (scheme == "tuic") {
    error = Tuic(*uri, out);
  } else if (scheme == "anytls") {
    error = AnyTls(*uri, out);
  } else if (scheme == "socks" || scheme == "socks5" || scheme == "socks4" || scheme == "socks4a") {
    error = Socks(*uri, out);
  } else if (scheme == "http" || scheme == "https") {
    error = HttpProxy(*uri, out);
  } else if (scheme == "naive+https" || scheme == "naive+quic") {
    error = Naive(*uri, out);
  } else if (scheme == "wireguard" || scheme == "wg") {
    error = WireGuard(*uri, out);
  } else if (scheme == "ssh") {
    error = Ssh(*uri, out);
  } else if (scheme == "snell") {
    error = Snell(*uri, out);
  } else if (scheme == "ssr") {
    error = "ShadowsocksR sing-box больше не поддерживает";
  } else {
    error = "схема " + scheme + " не поддерживается";
  }
  if (!error.empty()) {
    parsed.error = std::move(error);
    return parsed;
  }
  parsed.name = uri->name;
  if (out.contains("tag")) {  // vmess carries its name inside
    if (parsed.name.empty() && out["tag"].is_string()) {
      parsed.name = out["tag"].get<std::string>();
    }
    out.erase("tag");
  }
  if (parsed.name.empty()) {
    const std::string server = out.contains("peers") ? uri->host : out.value("server", std::string());
    parsed.name = server.find(':') != std::string::npos ? "[" + server + "]" : server;
    // server_port 0: ranges only (port hopping) - the link's own text then.
    if (const int port = out.value("server_port", 0); port != 0) {
      parsed.name += ":" + std::to_string(port);
    } else if (!uri->port.empty()) {
      parsed.name += ":" + uri->port;
    }
  }
  // Every value came from a string that may not be UTF-8; dumping must not throw.
  parsed.outbound = out.dump(-1, ' ', false, Json::error_handler_t::replace);
  return parsed;
}

LinksConfig BuildConfigFromLinks(const std::vector<std::string>& links) {
  ImportItems items;
  items.links = links;
  return BuildConfig(items);
}

LinksConfig BuildConfig(const ImportItems& items) {
  LinksConfig result;
  result.found = items.links.size() + items.outbounds.size() + items.skipped.size();
  result.errors = items.skipped;
  Json servers = Json::array();
  Json endpoints = Json::array();
  std::vector<std::string> tags;
  const auto add = [&](Json outbound, const std::string& name) {
    // Tags are unique in a config: a repeated name gets a number.
    const std::string base = name.empty() ? std::string("server") : name;
    std::string tag = base;
    for (int n = 2; std::find(tags.begin(), tags.end(), tag) != tags.end() || tag == "proxy" || tag == "auto" ||
                    tag == "direct";
         ++n) {
      tag = std::format("{} {}", base, n);
    }
    Json tagged = Json::object();
    tagged["type"] = outbound["type"];
    tagged["tag"] = tag;
    for (auto it = outbound.begin(); it != outbound.end(); ++it) {
      if (it.key() != "type" && it.key() != "tag") {
        tagged[it.key()] = it.value();
      }
    }
    // WireGuard is an endpoint in sing-box; selectors and routes take its tag all the same.
    (tagged["type"] == "wireguard" ? endpoints : servers).push_back(std::move(tagged));
    tags.push_back(tag);
  };
  for (std::size_t i = 0; i < items.links.size(); ++i) {
    const ParsedLink parsed = ParseShareLink(items.links[i]);
    if (!parsed.outbound) {
      result.errors.push_back(LinkLabel(items.links[i], i) + ": " + parsed.error);
      continue;
    }
    add(Json::parse(*parsed.outbound), parsed.name);
  }
  for (const std::string& text : items.outbounds) {
    Json outbound = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!outbound.is_object() || !outbound.contains("type") || !outbound["type"].is_string()) {
      continue;
    }
    const auto tag = outbound.find("tag");
    const std::string name = tag != outbound.end() && tag->is_string() ? tag->get<std::string>() : std::string();
    add(std::move(outbound), name);
  }
  result.servers = tags.size();
  if (tags.empty()) {
    return result;
  }

  Json config = Json::object();
  config["log"] = Json{{"level", "info"}, {"timestamp", true}};
  config["dns"] = Json{
      {"servers", Json::array({Json{{"type", "https"}, {"tag", "remote"}, {"server", "1.1.1.1"}, {"detour", "proxy"}},
                               Json{{"type", "local"}, {"tag", "local"}}})},
      {"final", "remote"},
  };
  config["inbounds"] = Json::array({Json{{"type", "tun"},
                                         {"tag", "tun-in"},
                                         {"address", Json::array({"172.19.0.1/30", "fdfe:dcba:9876::1/126"})},
                                         {"auto_route", true},
                                         {"strict_route", true}}});
  if (!endpoints.empty()) {
    config["endpoints"] = std::move(endpoints);
  }
  Json outbounds = Json::array();
  Json selector = {{"type", "selector"}, {"tag", "proxy"}};
  if (tags.size() > 1) {
    std::vector<std::string> options = {"auto"};
    options.insert(options.end(), tags.begin(), tags.end());
    selector["outbounds"] = options;
    selector["default"] = "auto";
    outbounds.push_back(selector);
    outbounds.push_back(Json{{"type", "urltest"}, {"tag", "auto"}, {"outbounds", tags}});
  } else {
    selector["outbounds"] = tags;
    outbounds.push_back(selector);
  }
  for (auto& server : servers) {
    outbounds.push_back(std::move(server));
  }
  outbounds.push_back(Json{{"type", "direct"}, {"tag", "direct"}});
  config["outbounds"] = std::move(outbounds);
  config["route"] = Json{
      {"rules", Json::array({Json{{"action", "sniff"}}, Json{{"protocol", "dns"}, {"action", "hijack-dns"}},
                             Json{{"ip_is_private", true}, {"outbound", "direct"}}})},
      {"final", "proxy"},
      {"auto_detect_interface", true},
      {"default_domain_resolver", "local"},
  };
  result.config = config.dump(2, ' ', false, Json::error_handler_t::replace);
  return result;
}

LinksConfig ConfigFromSubscription(std::string_view body) {
  const ImportItems items = RecognizeImport(body);
  if (items.json) {
    LinksConfig result;
    result.config = *items.json;
    return result;
  }
  if (items.Servers() || !items.skipped.empty()) {
    return BuildConfig(items);
  }
  return {};
}

}  // namespace sovereign::tray
