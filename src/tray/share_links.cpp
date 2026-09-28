#include "share_links.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <utility>

namespace sovereign::tray {

namespace {

using Json = nlohmann::ordered_json;

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
constexpr std::array<std::string_view, 7> kProxySchemes = {"hysteria2", "trojan", "vless", "vmess", "tuic", "hy2", "ss"};

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

// The TLS block the V2Ray-style parameters describe; null when there's none.
Json Tls(const std::string& security, const Uri& uri, const std::string& sni, const std::string& alpn,
         const std::string& fingerprint, bool insecure) {
  if (security != "tls" && security != "reality" && security != "xtls") {
    return nullptr;
  }
  Json tls = Json::object();
  tls["enabled"] = true;
  if (!sni.empty()) {
    tls["server_name"] = sni;
  }
  if (insecure) {
    tls["insecure"] = true;
  }
  if (const auto list = SplitList(alpn, ','); !list.empty()) {
    tls["alpn"] = list;
  }
  std::string fp = fingerprint;
  if (security == "reality") {
    if (fp.empty()) {
      fp = "chrome";  // REALITY runs over uTLS in sing-box
    }
    Json reality = Json::object();
    reality["enabled"] = true;
    reality["public_key"] = uri.Get("pbk");
    if (const std::string sid = uri.Get("sid"); !sid.empty()) {
      reality["short_id"] = sid;
    }
    tls["reality"] = std::move(reality);
  }
  if (!fp.empty() && fp != "none") {
    tls["utls"] = Json{{"enabled", true}, {"fingerprint", fp}};
  }
  return tls;
}

// The transport for a V2Ray `type`/`net`; an error for the ones sing-box
// doesn't have.
std::pair<Json, std::string> Transport(std::string type, const std::string& host, std::string path,
                                       const std::string& serviceName, const std::string& headerType) {
  type = LowerCopy(type);
  if (type.empty() || type == "tcp" || type == "raw") {
    if (LowerCopy(headerType) == "http") {
      Json http = {{"type", "http"}};
      if (const auto hosts = SplitList(host, ','); !hosts.empty()) {
        http["host"] = hosts;
      }
      if (!path.empty()) {
        http["path"] = path;
      }
      return {http, {}};
    }
    return {nullptr, {}};
  }
  if (type == "ws" || type == "websocket") {
    Json ws = {{"type", "ws"}};
    // "/path?ed=2048": early data, which sing-box takes as options.
    if (const std::size_t q = path.find("?ed="); q != std::string::npos) {
      const auto early = ParseInt(std::string_view(path).substr(q + 4));
      path.resize(q);
      if (early && *early > 0) {
        ws["max_early_data"] = *early;
        ws["early_data_header_name"] = "Sec-WebSocket-Protocol";
      }
    }
    ws["path"] = path.empty() ? "/" : path;
    if (!host.empty()) {
      ws["headers"] = Json{{"Host", host}};
    }
    return {ws, {}};
  }
  if (type == "grpc" || type == "gun") {
    Json grpc = {{"type", "grpc"}};
    if (!serviceName.empty()) {
      grpc["service_name"] = serviceName;
    }
    return {grpc, {}};
  }
  if (type == "http" || type == "h2") {
    Json http = {{"type", "http"}};
    if (const auto hosts = SplitList(host, ','); !hosts.empty()) {
      http["host"] = hosts;
    }
    if (!path.empty()) {
      http["path"] = path;
    }
    return {http, {}};
  }
  if (type == "httpupgrade") {
    Json upgrade = {{"type", "httpupgrade"}};
    if (!host.empty()) {
      upgrade["host"] = host;
    }
    upgrade["path"] = path.empty() ? "/" : path;
    return {upgrade, {}};
  }
  if (type == "quic") {
    return {Json{{"type", "quic"}}, {}};
  }
  return {nullptr, "транспорт " + type + " sing-box не поддерживает"};
}

bool SetServer(Json& out, const Uri& uri, std::string& error) {
  const auto port = ParsePort(uri.port);
  if (uri.host.empty() || !port) {
    error = "нет адреса или порта сервера";
    return false;
  }
  out["server"] = uri.host;
  out["server_port"] = *port;
  return true;
}

void AddTransportAndTls(Json& out, const Uri& uri, const std::string& defaultSecurity, std::string& error) {
  const std::string security = LowerCopy(uri.Get("security", defaultSecurity));
  const std::string host = uri.Get("host");
  const Json tls = Tls(security, uri, uri.Get("sni", uri.Get("peer")), uri.Get("alpn"), uri.Get("fp"),
                       uri.Flag("allowinsecure") || uri.Flag("insecure"));
  if (!tls.is_null()) {
    out["tls"] = tls;
  }
  auto [transport, transportError] =
      Transport(uri.Get("type", "tcp"), host, uri.Get("path"), uri.Get("servicename"), uri.Get("headertype"));
  if (!transportError.empty()) {
    error = std::move(transportError);
    return;
  }
  if (!transport.is_null()) {
    out["transport"] = std::move(transport);
  }
}

std::string Vless(const Uri& uri, Json& out) {
  std::string error;
  out["type"] = "vless";
  if (!SetServer(out, uri, error)) {
    return error;
  }
  out["uuid"] = PercentDecode(uri.userinfo);
  if (const std::string flow = uri.Get("flow"); !flow.empty()) {
    out["flow"] = flow;
  }
  AddTransportAndTls(out, uri, "none", error);
  out["packet_encoding"] = "xudp";
  if (PercentDecode(uri.userinfo).empty()) {
    return "нет UUID";
  }
  return error;
}

std::string Trojan(const Uri& uri, Json& out) {
  std::string error;
  out["type"] = "trojan";
  if (!SetServer(out, uri, error)) {
    return error;
  }
  out["password"] = PercentDecode(uri.userinfo);
  AddTransportAndTls(out, uri, "tls", error);
  return error;
}

// vmess://base64(JSON) - V2RayN's format; some clients write it like vless.
std::string Vmess(const Uri& uri, Json& out) {
  const auto decoded = DecodeBase64(uri.body);
  const auto json = decoded ? nlohmann::json::parse(*decoded, nullptr, false) : nlohmann::json();
  if (!json.is_object()) {
    std::string error;
    out["type"] = "vmess";
    if (!SetServer(out, uri, error)) {
      return error;
    }
    out["uuid"] = PercentDecode(uri.userinfo);
    out["security"] = uri.Get("encryption", "auto");
    AddTransportAndTls(out, uri, "none", error);
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
  out["type"] = "vmess";
  Uri server;
  server.host = str("add");
  server.port = str("port");
  std::string error;
  if (!SetServer(out, server, error)) {
    return error;
  }
  out["uuid"] = str("id");
  const std::string scy = str("scy");
  out["security"] = scy.empty() ? "auto" : scy;
  if (const auto alterId = ParseInt(str("aid")); alterId && *alterId > 0) {
    out["alter_id"] = *alterId;
  }
  const std::string net = str("net");
  const Json tls = Tls(LowerCopy(str("tls")), server, str("sni"), str("alpn"), str("fp"), false);
  if (!tls.is_null()) {
    out["tls"] = tls;
  }
  auto [transport, transportError] =
      Transport(net, str("host"), str("path"), LowerCopy(net) == "grpc" ? str("path") : std::string(), str("type"));
  if (!transportError.empty()) {
    return transportError;
  }
  if (!transport.is_null()) {
    out["transport"] = std::move(transport);
  }
  if (const std::string ps = str("ps"); !ps.empty()) {
    out["tag"] = ps;  // the name - taken out again by the caller
  }
  if (str("id").empty()) {
    return "нет UUID";
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
  out["type"] = "shadowsocks";
  std::string error;
  if (!SetServer(out, server, error)) {
    return error;
  }
  out["method"] = LowerCopy(credentials.substr(0, colon));
  out["password"] = credentials.substr(colon + 1);
  if (const std::string plugin = uri.Get("plugin"); !plugin.empty()) {
    const std::size_t semi = plugin.find(';');
    std::string name = plugin.substr(0, semi);
    if (name == "simple-obfs") {
      name = "obfs-local";
    }
    if (name != "obfs-local" && name != "v2ray-plugin") {
      return "плагин " + name + " sing-box не поддерживает";
    }
    out["plugin"] = name;
    if (semi != std::string::npos) {
      out["plugin_opts"] = plugin.substr(semi + 1);
    }
  }
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

std::string Hysteria2(const Uri& uri, Json& out) {
  out["type"] = "hysteria2";
  if (uri.host.empty()) {
    return "нет адреса сервера";
  }
  out["server"] = uri.host;
  const std::string ports = uri.Get("mport");
  if (const auto single = ParsePort(uri.port); single && ports.empty()) {
    out["server_port"] = *single;
  } else {
    const auto ranges = PortRanges(ports.empty() ? uri.port : ports);
    if (ranges.empty()) {
      return "нет порта сервера";
    }
    if (single) {
      out["server_port"] = *single;
    }
    out["server_ports"] = ranges;
  }
  out["password"] = PercentDecode(uri.userinfo);
  if (const std::string obfs = uri.Get("obfs"); !obfs.empty() && obfs != "none") {
    out["obfs"] = Json{{"type", obfs}, {"password", uri.Get("obfs-password")}};
  }
  Json tls = Json::object();
  tls["enabled"] = true;
  if (const std::string sni = uri.Get("sni"); !sni.empty()) {
    tls["server_name"] = sni;
  }
  if (uri.Flag("insecure") || uri.Flag("allowinsecure")) {
    tls["insecure"] = true;
  }
  if (const auto alpn = SplitList(uri.Get("alpn"), ','); !alpn.empty()) {
    tls["alpn"] = alpn;
  }
  out["tls"] = std::move(tls);
  return {};
}

std::string Tuic(const Uri& uri, Json& out) {
  out["type"] = "tuic";
  std::string error;
  if (!SetServer(out, uri, error)) {
    return error;
  }
  const std::string credentials = PercentDecode(uri.userinfo);
  const std::size_t colon = credentials.find(':');
  out["uuid"] = credentials.substr(0, colon);
  if (colon != std::string::npos) {
    out["password"] = credentials.substr(colon + 1);
  }
  if (const std::string cc = uri.Get("congestion_control", uri.Get("congestion-control")); !cc.empty()) {
    out["congestion_control"] = cc;
  }
  if (const std::string mode = uri.Get("udp_relay_mode", uri.Get("udp-relay-mode")); !mode.empty()) {
    out["udp_relay_mode"] = mode;
  }
  Json tls = Json::object();
  tls["enabled"] = true;
  if (const std::string sni = uri.Get("sni"); !sni.empty()) {
    tls["server_name"] = sni;
  }
  if (uri.Flag("allow_insecure") || uri.Flag("insecure") || uri.Flag("allowinsecure")) {
    tls["insecure"] = true;
  }
  if (uri.Flag("disable_sni")) {
    tls["disable_sni"] = true;
  }
  const auto alpn = SplitList(uri.Get("alpn"), ',');
  tls["alpn"] = alpn.empty() ? std::vector<std::string>{"h3"} : alpn;
  out["tls"] = std::move(tls);
  if (colon == 0 || credentials.empty()) {
    return "нет UUID";
  }
  return {};
}

// Where a scheme starts that ends right before `sep` ("://"), or npos: the
// longest proxy scheme the letters before it end with.
std::size_t SchemeStart(std::string_view text, std::size_t sep, bool withHttps) {
  std::size_t begin = sep;
  while (begin > 0 && IsAlnum(text[begin - 1])) {
    --begin;
  }
  const std::string run = LowerCopy(text.substr(begin, sep - begin));
  for (const std::string_view scheme : kProxySchemes) {
    if (run.ends_with(scheme)) {
      return sep - scheme.size();
    }
  }
  if (withHttps) {
    for (const std::string_view scheme : {std::string_view("https"), std::string_view("http")}) {
      // A web link starts its own word: inside a key it's a parameter's value.
      if (run == scheme) {
        return begin;
      }
    }
  }
  return std::string_view::npos;
}

struct Found {
  std::size_t start = 0;
  bool proxy = false;
};

// Every scheme start in `text`, proxy links and web links alike.
std::vector<Found> FindStarts(std::string_view text) {
  std::vector<Found> starts;
  std::size_t from = 0;
  while (true) {
    const std::size_t sep = text.find("://", from);
    if (sep == std::string_view::npos) {
      break;
    }
    const std::size_t start = SchemeStart(text, sep, true);
    if (start != std::string_view::npos) {
      const bool web = LowerCopy(text.substr(start, sep - start)).starts_with("http");
      // Not a parameter's value inside a key ("...&spx=https://...").
      const bool wordStart = start == 0 || IsSpace(text[start - 1]) ||
                             std::string_view("\"'<>(),;").find(text[start - 1]) != std::string_view::npos;
      if (!web || wordStart) {
        starts.push_back({start, !web});
      }
    }
    from = sep + 3;
  }
  return starts;
}

// One link's text: from its start to the next link's; whitespace ends it
// before the name, a line ends the name.
std::string_view LinkAt(std::string_view text, std::size_t start, std::size_t end) {
  std::string_view link = text.substr(start, end - start);
  const std::size_t hash = link.find('#');
  const std::size_t space = link.find_first_of(" \t\r\n\f\v\"'<>");
  if (space != std::string_view::npos && (hash == std::string_view::npos || space < hash)) {
    link = link.substr(0, space);
  } else if (hash != std::string_view::npos) {
    link = link.substr(0, link.find_first_of("\r\n", hash));
  }
  // A list's separator glued to the next key: "vless://...,vless://...".
  link = Trim(link);
  while (!link.empty() && (link.back() == ',' || link.back() == ';')) {
    link.remove_suffix(1);
  }
  return link;
}

void Collect(std::string_view text, ImportItems& items, int depth) {
  const auto starts = FindStarts(text);
  for (std::size_t i = 0; i < starts.size(); ++i) {
    const std::size_t end = i + 1 < starts.size() ? starts[i + 1].start : text.size();
    const std::string_view link = LinkAt(text, starts[i].start, end);
    if (link.find("://") + 3 >= link.size()) {
      continue;  // a scheme and nothing after it
    }
    auto& into = starts[i].proxy ? items.links : items.urls;
    if (std::find(into.begin(), into.end(), link) == into.end()) {
      into.emplace_back(link);
    }
  }
  // Nothing readable: maybe the whole thing is base64, as subscriptions send.
  if (starts.empty() && depth == 0) {
    if (const auto decoded = DecodeBase64(text); decoded && decoded->find("://") != std::string::npos) {
      Collect(*decoded, items, depth + 1);
    }
  }
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
  return items.links;
}

ImportItems RecognizeImport(std::string_view text) {
  ImportItems items;
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3);  // a BOM from Notepad
  }
  text = Trim(text);
  if (text.starts_with('{')) {
    try {
      const auto json = nlohmann::json::parse(text);
      if (json.is_object()) {
        items.json = std::string(text);
        return items;
      }
    } catch (const nlohmann::json::parse_error& e) {
      // Its message says where: "at line 3, column 7".
      items.jsonError = e.what();
    }
  }
  Collect(text, items, 0);
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
  if (uri->scheme == "vless") {
    error = Vless(*uri, out);
  } else if (uri->scheme == "vmess") {
    error = Vmess(*uri, out);
  } else if (uri->scheme == "trojan") {
    error = Trojan(*uri, out);
  } else if (uri->scheme == "ss") {
    error = Shadowsocks(*uri, out);
  } else if (uri->scheme == "hysteria2" || uri->scheme == "hy2") {
    error = Hysteria2(*uri, out);
  } else if (uri->scheme == "tuic") {
    error = Tuic(*uri, out);
  } else {
    error = "схема " + uri->scheme + " не поддерживается";
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
    const std::string server = out.value("server", std::string());
    parsed.name = server.find(':') != std::string::npos ? "[" + server + "]" : server;
    if (out.contains("server_port")) {
      parsed.name += ":" + std::to_string(out["server_port"].get<int>());
    }
  }
  // Every value came from a string that may not be UTF-8; dumping must not throw.
  parsed.outbound = out.dump(-1, ' ', false, Json::error_handler_t::replace);
  return parsed;
}

LinksConfig BuildConfigFromLinks(const std::vector<std::string>& links) {
  LinksConfig result;
  Json servers = Json::array();
  std::vector<std::string> tags;
  for (std::size_t i = 0; i < links.size(); ++i) {
    const ParsedLink parsed = ParseShareLink(links[i]);
    if (!parsed.outbound) {
      result.errors.push_back(std::format("ключ {}: {}", i + 1, parsed.error));
      continue;
    }
    // Tags are unique in a config: a repeated name gets a number.
    std::string tag = parsed.name;
    for (int n = 2; std::find(tags.begin(), tags.end(), tag) != tags.end() || tag == "proxy" || tag == "auto" ||
                    tag == "direct";
         ++n) {
      tag = std::format("{} {}", parsed.name, n);
    }
    Json outbound = Json::parse(*parsed.outbound);
    Json tagged = Json::object();
    tagged["type"] = outbound["type"];
    tagged["tag"] = tag;
    for (auto it = outbound.begin(); it != outbound.end(); ++it) {
      if (it.key() != "type") {
        tagged[it.key()] = it.value();
      }
    }
    servers.push_back(std::move(tagged));
    tags.push_back(tag);
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

}  // namespace sovereign::tray
