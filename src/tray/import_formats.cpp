#include "import_formats.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace sovereign::tray::detail {

namespace {

// YAML turned into JSON: deep enough for any Clash file, and an anchor
// referred to over and over (a "billion laughs") stops at the budget.
constexpr int kMaxYamlDepth = 64;
constexpr std::size_t kMaxYamlNodes = 200'000;

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
  return text;
}

const Json& Null() {
  static const Json null;
  return null;
}

const Json& Member(const Json& object, const char* key) {
  if (!object.is_object()) {
    return Null();
  }
  const auto it = object.find(key);
  return it == object.end() ? Null() : *it;
}

// A value as text, whatever JSON type it came as (YAML gives strings only).
std::string Text(const Json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer() || value.is_number_unsigned() || value.is_boolean()) {
    return value.dump();
  }
  return {};
}

std::string Str(const Json& object, const char* key) { return Text(Member(object, key)); }

bool Bool(const Json& object, const char* key) {
  const Json& value = Member(object, key);
  if (value.is_boolean()) {
    return value.get<bool>();
  }
  const std::string text = Lower(Text(value));
  return text == "true" || text == "1" || text == "yes";
}

// A list of scalars, or one scalar.
std::vector<std::string> List(const Json& object, const char* key) {
  const Json& value = Member(object, key);
  std::vector<std::string> out;
  if (value.is_array()) {
    for (const Json& item : value) {
      if (std::string text = Text(item); !text.empty()) {
        out.push_back(std::move(text));
      }
    }
  } else if (std::string text = Text(value); !text.empty()) {
    out.push_back(std::move(text));
  }
  return out;
}

std::string Join(const std::vector<std::string>& items) {
  std::string out;
  for (const std::string& item : items) {
    out += (out.empty() ? "" : ",") + item;
  }
  return out;
}

const Json& First(const Json& value) {
  return value.is_array() && !value.empty() ? value.front() : value;
}

// A share link put together from its parts, each encoded as a link needs.
struct LinkParts {
  std::string scheme;  // empty: not a server (Clash's direct, Xray's freedom)
  std::string userinfo;
  std::string host;
  std::string port;
  std::vector<std::pair<std::string, std::string>> query;
  std::string name;

  void Add(std::string key, std::string value) {
    if (!value.empty()) {
      query.emplace_back(std::move(key), std::move(value));
    }
  }
};

std::string Encode(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : text) {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                       c == '.' || c == '_' || c == '~';
    if (plain) {
      out.push_back(c);
    } else {
      const auto byte = static_cast<unsigned char>(c);
      out += {'%', kHex[byte >> 4], kHex[byte & 15]};
    }
  }
  return out;
}

std::string MakeLink(const LinkParts& parts) {
  std::string link = parts.scheme + "://";
  if (!parts.userinfo.empty()) {
    link += Encode(parts.userinfo) + "@";
  }
  link += parts.host.find(':') != std::string::npos ? "[" + parts.host + "]" : parts.host;
  if (!parts.port.empty()) {
    link += ":" + parts.port;
  }
  for (std::size_t i = 0; i < parts.query.size(); ++i) {
    link += (i == 0 ? "?" : "&") + parts.query[i].first + "=" + Encode(parts.query[i].second);
  }
  if (!parts.name.empty()) {
    link += "#" + Encode(parts.name);
  }
  return link;
}

// Where a server goes: a link, or a reason on the skipped list.
void Deliver(const LinkParts& parts, const std::string& error, ImportItems& items) {
  if (!error.empty()) {
    items.skipped.push_back((parts.name.empty() ? parts.host : parts.name) + ": " + error);
  } else if (!parts.scheme.empty()) {
    items.links.push_back(MakeLink(parts));
  }
}

// ---- Clash / Mihomo ----

void ClashTls(const Json& p, LinkParts& link, bool tlsByDefault) {
  const Json& reality = Member(p, "reality-opts");
  const bool tls = tlsByDefault || Bool(p, "tls");
  link.Add("security", reality.is_object() ? "reality" : tls ? "tls" : "none");
  std::string sni = Str(p, "servername");
  if (sni.empty()) {
    sni = Str(p, "sni");
  }
  link.Add("sni", sni);
  link.Add("fp", Str(p, "client-fingerprint"));
  link.Add("alpn", Join(List(p, "alpn")));
  if (Bool(p, "skip-cert-verify")) {
    link.Add("allowInsecure", "1");
  }
  link.Add("pbk", Str(reality, "public-key"));
  link.Add("sid", Str(reality, "short-id"));
}

void ClashTransport(const Json& p, LinkParts& link) {
  const std::string network = Lower(Str(p, "network"));
  if (network == "ws") {
    const Json& ws = Member(p, "ws-opts");
    std::string path = Str(ws, "path");
    const std::string early = Str(ws, "max-early-data");
    if (!early.empty() && early != "0" && path.find("?ed=") == std::string::npos) {
      path += "?ed=" + early;
    }
    link.Add("type", Bool(ws, "v2ray-http-upgrade") ? "httpupgrade" : "ws");
    link.Add("path", path);
    link.Add("host", Str(Member(ws, "headers"), "Host"));
  } else if (network == "grpc") {
    link.Add("type", "grpc");
    link.Add("serviceName", Str(Member(p, "grpc-opts"), "grpc-service-name"));
  } else if (network == "h2") {
    const Json& h2 = Member(p, "h2-opts");
    link.Add("type", "http");
    link.Add("host", Join(List(h2, "host")));
    link.Add("path", Str(h2, "path"));
  } else if (network == "http") {
    // Clash's "http" network is the HTTP header over TCP.
    const Json& http = Member(p, "http-opts");
    link.Add("type", "tcp");
    link.Add("headerType", "http");
    const auto paths = List(http, "path");
    link.Add("path", paths.empty() ? std::string() : paths.front());
    link.Add("host", Join(List(Member(http, "headers"), "Host")));
  } else if (!network.empty() && network != "tcp") {
    link.Add("type", network);  // xhttp and the like: the parser says sing-box has none
  }
}

std::string ClashShadowsocksPlugin(const Json& p, LinkParts& link) {
  const std::string plugin = Lower(Str(p, "plugin"));
  if (plugin.empty()) {
    return {};
  }
  const Json& opts = Member(p, "plugin-opts");
  if (plugin == "obfs") {
    std::string value = "obfs-local;obfs=" + Str(opts, "mode");
    if (const std::string host = Str(opts, "host"); !host.empty()) {
      value += ";obfs-host=" + host;
    }
    link.Add("plugin", value);
    return {};
  }
  if (plugin == "v2ray-plugin") {
    std::string value = "v2ray-plugin;mode=" + (Str(opts, "mode").empty() ? std::string("websocket") : Str(opts, "mode"));
    if (Bool(opts, "tls")) {
      value += ";tls";
    }
    if (const std::string host = Str(opts, "host"); !host.empty()) {
      value += ";host=" + host;
    }
    if (const std::string path = Str(opts, "path"); !path.empty()) {
      value += ";path=" + path;
    }
    if (Bool(opts, "mux")) {
      value += ";mux=4";
    }
    link.Add("plugin", value);
    return {};
  }
  return "плагин " + plugin + " sing-box не поддерживает";
}

// Clash's WireGuard: the peer on the proxy itself, or the first of `peers`.
std::string ClashWireGuard(const Json& p, LinkParts& link) {
  link.scheme = "wireguard";
  link.userinfo = Str(p, "private-key");
  const Json& peers = Member(p, "peers");
  const Json& peer = peers.is_array() && !peers.empty() ? peers.front() : p;
  if (&peer != &p) {
    link.host = Str(peer, "server");
    link.port = Str(peer, "port");
  }
  link.Add("publickey", Str(peer, "public-key"));
  link.Add("presharedkey", Str(peer, "pre-shared-key"));
  std::vector<std::string> address;
  for (const char* key : {"ip", "ipv6"}) {
    if (std::string ip = Str(p, key); !ip.empty()) {
      address.push_back(std::move(ip));
    }
  }
  link.Add("address", Join(address));
  link.Add("mtu", Str(p, "mtu"));
  const Json& reserved = Member(peer, "reserved");
  link.Add("reserved", reserved.is_array() ? Join(List(peer, "reserved")) : Text(reserved));
  link.Add("allowedips", Join(List(peer, "allowed-ips")));
  return {};
}

std::string ClashProxy(const Json& p, LinkParts& link) {
  const std::string type = Lower(Str(p, "type"));
  link.name = Str(p, "name");
  link.host = Str(p, "server");
  link.port = Str(p, "port");
  const std::string password = Str(p, "password");
  if (type == "vless" || type == "vmess") {
    link.scheme = type;
    link.userinfo = Str(p, "uuid");
    if (type == "vless") {
      link.Add("flow", Str(p, "flow"));
    } else {
      link.Add("encryption", Str(p, "cipher"));
      link.Add("aid", Str(p, "alterId"));
    }
    ClashTls(p, link, false);
    ClashTransport(p, link);
    return {};
  }
  if (type == "trojan") {
    link.scheme = "trojan";
    link.userinfo = password;
    ClashTls(p, link, true);
    ClashTransport(p, link);
    return {};
  }
  if (type == "ss") {
    link.scheme = "ss";
    link.userinfo = Str(p, "cipher") + ":" + password;
    return ClashShadowsocksPlugin(p, link);
  }
  if (type == "hysteria2" || type == "hy2" || type == "hysteria") {
    link.scheme = type == "hysteria" ? "hysteria" : "hysteria2";
    if (type == "hysteria") {
      link.Add("auth", Str(p, "auth-str").empty() ? Str(p, "auth") : Str(p, "auth-str"));
      link.Add("upmbps", Str(p, "up"));
      link.Add("downmbps", Str(p, "down"));
      link.Add("protocol", Str(p, "protocol"));
      link.Add("obfsParam", Str(p, "obfs"));
    } else {
      link.userinfo = password.empty() ? Str(p, "auth") : password;
      link.Add("obfs", Str(p, "obfs"));
      link.Add("obfs-password", Str(p, "obfs-password"));
    }
    link.Add("mport", Str(p, "ports"));
    link.Add("sni", Str(p, "sni"));
    link.Add("alpn", Join(List(p, "alpn")));
    if (Bool(p, "skip-cert-verify")) {
      link.Add("insecure", "1");
    }
    return {};
  }
  if (type == "tuic") {
    if (Str(p, "uuid").empty() && !Str(p, "token").empty()) {
      return "TUIC v4 sing-box не поддерживает";
    }
    link.scheme = "tuic";
    link.userinfo = Str(p, "uuid") + ":" + password;
    link.Add("congestion_control", Str(p, "congestion-controller"));
    link.Add("udp_relay_mode", Str(p, "udp-relay-mode"));
    link.Add("sni", Str(p, "sni"));
    link.Add("alpn", Join(List(p, "alpn")));
    if (Bool(p, "disable-sni")) {
      link.Add("disable_sni", "1");
    }
    if (Bool(p, "skip-cert-verify")) {
      link.Add("insecure", "1");
    }
    return {};
  }
  if (type == "anytls") {
    link.scheme = "anytls";
    link.userinfo = password;
    ClashTls(p, link, true);
    return {};
  }
  if (type == "socks5" || type == "http") {
    const std::string user = Str(p, "username");
    link.userinfo = user.empty() && password.empty() ? std::string() : user + ":" + password;
    if (type == "socks5") {
      if (Bool(p, "tls")) {
        return "SOCKS через TLS sing-box не поддерживает";
      }
      link.scheme = "socks5";
      return {};
    }
    link.scheme = Bool(p, "tls") ? "https" : "http";
    link.Add("sni", Str(p, "sni"));
    if (Bool(p, "skip-cert-verify")) {
      link.Add("insecure", "1");
    }
    return {};
  }
  if (type == "wireguard") {
    return ClashWireGuard(p, link);
  }
  if (type == "ssh") {
    link.scheme = "ssh";
    link.userinfo = Str(p, "username") + ":" + password;
    link.Add("pk", Str(p, "private-key"));
    link.Add("hk", Join(List(p, "host-key")));
    return {};
  }
  if (type == "snell") {
    link.scheme = "snell";
    link.userinfo = Str(p, "psk");
    link.Add("version", Str(p, "version"));
    const Json& obfs = Member(p, "obfs-opts");
    link.Add("obfs", Str(obfs, "mode"));
    link.Add("obfs-host", Str(obfs, "host"));
    return {};
  }
  if (type == "ssr") {
    return "ShadowsocksR sing-box больше не поддерживает";
  }
  if (type == "direct" || type == "dns" || type == "reject" || type == "pass" || type.empty()) {
    return {};  // not a server
  }
  return "тип " + type + " sing-box не поддерживает";
}

void ImportClash(const Json& document, ImportItems& items) {
  const Json& proxies = Member(document, "proxies");
  if (proxies.is_array()) {
    for (const Json& proxy : proxies) {
      if (proxy.is_object()) {
        LinkParts link;
        const std::string error = ClashProxy(proxy, link);
        Deliver(link, error, items);
      }
    }
  }
  // A provider is another subscription: its link.
  const Json& providers = Member(document, "proxy-providers");
  if (providers.is_object()) {
    for (const auto& provider : providers.items()) {
      if (const std::string url = Str(provider.value(), "url"); url.starts_with("http")) {
        if (std::find(items.urls.begin(), items.urls.end(), url) == items.urls.end()) {
          items.urls.push_back(url);
        }
      }
    }
  }
}

// ---- Xray / V2Ray ----

void XrayStream(const Json& outbound, LinkParts& link) {
  const Json& stream = Member(outbound, "streamSettings");
  const std::string security = Lower(Str(stream, "security"));
  if (security == "tls" || security == "reality" || security == "xtls") {
    const Json& tls = Member(stream, security == "reality" ? "realitySettings" : "tlsSettings");
    link.Add("security", security == "xtls" ? "tls" : security);
    link.Add("sni", Str(tls, "serverName"));
    link.Add("fp", Str(tls, "fingerprint"));
    link.Add("alpn", Join(List(tls, "alpn")));
    if (Bool(tls, "allowInsecure")) {
      link.Add("allowInsecure", "1");
    }
    link.Add("pbk", Str(tls, "publicKey"));
    link.Add("sid", Str(tls, "shortId"));
  } else {
    link.Add("security", "none");
  }
  const std::string network = Lower(Str(stream, "network"));
  if (network == "ws" || network == "websocket") {
    const Json& ws = Member(stream, "wsSettings");
    link.Add("type", "ws");
    link.Add("path", Str(ws, "path"));
    std::string host = Str(ws, "host");
    link.Add("host", host.empty() ? Str(Member(ws, "headers"), "Host") : host);
  } else if (network == "grpc" || network == "gun") {
    link.Add("type", "grpc");
    link.Add("serviceName", Str(Member(stream, "grpcSettings"), "serviceName"));
  } else if (network == "h2" || network == "http") {
    const Json& http = Member(stream, "httpSettings");
    link.Add("type", "http");
    link.Add("host", Join(List(http, "host")));
    link.Add("path", Str(http, "path"));
  } else if (network == "httpupgrade") {
    const Json& upgrade = Member(stream, "httpupgradeSettings");
    link.Add("type", "httpupgrade");
    link.Add("path", Str(upgrade, "path"));
    link.Add("host", Str(upgrade, "host"));
  } else if (network.empty() || network == "tcp" || network == "raw") {
    const Json& tcp = Member(stream, network == "raw" ? "rawSettings" : "tcpSettings");
    const Json& header = Member(tcp, "header");
    if (Lower(Str(header, "type")) == "http") {
      const Json& request = Member(header, "request");
      link.Add("headerType", "http");
      const auto paths = List(request, "path");
      link.Add("path", paths.empty() ? std::string() : paths.front());
      link.Add("host", Join(List(Member(request, "headers"), "Host")));
    }
  } else {
    link.Add("type", network);  // kcp, xhttp: the parser says sing-box has none
  }
}

std::string XrayOutbound(const Json& outbound, const std::string& remarks, LinkParts& link) {
  const std::string protocol = Lower(Str(outbound, "protocol"));
  const Json& settings = Member(outbound, "settings");
  const std::string tag = Str(outbound, "tag");
  link.name = !remarks.empty() ? remarks : tag;
  if (protocol == "vless" || protocol == "vmess") {
    const Json& server = settings.contains("vnext") ? First(Member(settings, "vnext")) : settings;
    const Json& user = server.contains("users") ? First(Member(server, "users")) : server;
    link.scheme = protocol;
    link.host = Str(server, "address");
    link.port = Str(server, "port");
    link.userinfo = Str(user, "id");
    if (protocol == "vless") {
      link.Add("flow", Str(user, "flow"));
    } else {
      link.Add("encryption", Str(user, "security"));
      link.Add("aid", Str(user, "alterId"));
    }
    XrayStream(outbound, link);
    return {};
  }
  const Json& server = settings.contains("servers") ? First(Member(settings, "servers")) : settings;
  link.host = Str(server, "address");
  link.port = Str(server, "port");
  if (protocol == "trojan") {
    link.scheme = "trojan";
    link.userinfo = Str(server, "password");
    XrayStream(outbound, link);
    return {};
  }
  if (protocol == "shadowsocks") {
    link.scheme = "ss";
    link.userinfo = Str(server, "method") + ":" + Str(server, "password");
    return {};
  }
  if (protocol == "socks" || protocol == "http") {
    const Json& user = First(Member(server, "users"));
    const std::string name = Str(user, "user");
    const std::string pass = Str(user, "pass");
    link.scheme = protocol == "socks" ? "socks5" : "http";
    link.userinfo = name.empty() && pass.empty() ? std::string() : name + ":" + pass;
    return {};
  }
  if (protocol == "wireguard") {
    const Json& peer = First(Member(settings, "peers"));
    const std::string endpoint = Str(peer, "endpoint");
    const std::size_t colon = endpoint.rfind(':');
    link.scheme = "wireguard";
    link.host = endpoint.substr(0, colon);
    if (link.host.size() > 2 && link.host.front() == '[' && link.host.back() == ']') {
      link.host = link.host.substr(1, link.host.size() - 2);
    }
    link.port = colon == std::string::npos ? std::string() : endpoint.substr(colon + 1);
    link.userinfo = Str(settings, "secretKey");
    link.Add("publickey", Str(peer, "publicKey"));
    link.Add("presharedkey", Str(peer, "preSharedKey"));
    link.Add("address", Join(List(settings, "address")));
    link.Add("mtu", Str(settings, "mtu"));
    link.Add("reserved", Join(List(settings, "reserved")));
    link.Add("allowedips", Join(List(peer, "allowedIPs")));
    return {};
  }
  if (protocol == "freedom" || protocol == "blackhole" || protocol == "dns" || protocol == "loopback" ||
      protocol.empty()) {
    return {};  // not a server
  }
  return "протокол " + protocol + " sing-box не поддерживает";
}

void ImportXray(const Json& config, ImportItems& items) {
  const std::string remarks = Str(config, "remarks");
  std::size_t servers = 0;
  for (const Json& outbound : Member(config, "outbounds")) {
    LinkParts link;
    const std::string error = XrayOutbound(outbound, remarks, link);
    if (!link.scheme.empty() || !error.empty()) {
      ++servers;
    }
    // Several servers in one config under one "remarks": their tags tell them apart.
    if (servers > 1 && !remarks.empty() && !Str(outbound, "tag").empty()) {
      link.name = remarks + " " + Str(outbound, "tag");
    }
    Deliver(link, error, items);
  }
}

// ---- sing-box and SIP008 ----

void ImportSingBoxOutbound(const Json& outbound, ImportItems& items) {
  const std::string type = Str(outbound, "type");
  if (type.empty() || type == "selector" || type == "urltest" || type == "direct" || type == "block" ||
      type == "dns") {
    return;  // groups and the local ones: the config made around the servers has its own
  }
  items.outbounds.push_back(outbound.dump(-1, ' ', false, Json::error_handler_t::replace));
}

void ImportSip008(const Json& servers, ImportItems& items) {
  for (const Json& server : servers) {
    LinkParts link;
    link.scheme = "ss";
    link.host = Str(server, "server");
    link.port = Str(server, "server_port");
    link.userinfo = Str(server, "method") + ":" + Str(server, "password");
    link.name = Str(server, "remarks");
    if (std::string plugin = Str(server, "plugin"); !plugin.empty()) {
      if (const std::string opts = Str(server, "plugin_opts"); !opts.empty()) {
        plugin.append(";").append(opts);
      }
      link.Add("plugin", std::move(plugin));
    }
    Deliver(link, {}, items);
  }
}

bool HasMember(const Json& value, const char* key) { return value.is_object() && value.contains(key); }

void ImportValue(const Json& value, ImportItems& items, int depth) {
  if (depth > 2) {
    return;
  }
  if (value.is_array()) {
    for (const Json& item : value) {
      ImportValue(item, items, depth + 1);
    }
    return;
  }
  if (!value.is_object()) {
    return;
  }
  if (Member(value, "proxies").is_array() || Member(value, "proxy-providers").is_object()) {
    ImportClash(value, items);
    return;
  }
  if (const Json& outbounds = Member(value, "outbounds"); outbounds.is_array()) {
    const bool xray = std::any_of(outbounds.begin(), outbounds.end(),
                                  [](const Json& o) { return Member(o, "protocol").is_string(); });
    if (xray) {
      ImportXray(value, items);
      return;
    }
    // A sing-box config: whole (with inbounds) it's taken as it is; only
    // its servers, they get a config around them.
    const Json& inbounds = Member(value, "inbounds");
    if (!inbounds.is_array() || inbounds.empty()) {
      for (const Json& outbound : outbounds) {
        ImportSingBoxOutbound(outbound, items);
      }
      for (const Json& endpoint : Member(value, "endpoints")) {
        ImportSingBoxOutbound(endpoint, items);
      }
    }
    return;
  }
  if (const Json& servers = Member(value, "servers");
      servers.is_array() && !servers.empty() && HasMember(servers.front(), "method")) {
    ImportSip008(servers, items);
    return;
  }
  if (Member(value, "protocol").is_string() && HasMember(value, "settings")) {
    LinkParts link;
    const std::string error = XrayOutbound(value, Str(value, "remarks"), link);
    Deliver(link, error, items);
    return;
  }
  if (Member(value, "type").is_string() && (HasMember(value, "server") || HasMember(value, "peers"))) {
    ImportSingBoxOutbound(value, items);
  }
}

std::size_t Count(const ImportItems& items) {
  return items.links.size() + items.outbounds.size() + items.skipped.size() + items.urls.size();
}

// yaml-cpp's tree as JSON: scalars as strings, keys that aren't scalars dropped.
bool YamlToJson(const YAML::Node& node, Json& out, int depth, std::size_t& budget) {
  if (depth > kMaxYamlDepth || budget == 0) {
    return false;
  }
  --budget;
  switch (node.Type()) {
    case YAML::NodeType::Scalar:
      out = node.Scalar();
      return true;
    case YAML::NodeType::Sequence:
      out = Json::array();
      for (const YAML::Node& item : node) {
        Json converted;
        if (!YamlToJson(item, converted, depth + 1, budget)) {
          return false;
        }
        out.push_back(std::move(converted));
      }
      return true;
    case YAML::NodeType::Map:
      out = Json::object();
      for (const auto& entry : node) {
        if (!entry.first.IsScalar()) {
          continue;
        }
        Json converted;
        if (!YamlToJson(entry.second, converted, depth + 1, budget)) {
          return false;
        }
        out[entry.first.Scalar()] = std::move(converted);
      }
      return true;
    case YAML::NodeType::Null:
    case YAML::NodeType::Undefined:
      out = nullptr;
      return true;
  }
  return false;
}

}  // namespace

bool ImportJson(const Json& document, ImportItems& items) {
  ImportItems found;
  ImportValue(document, found, 0);
  if (Count(found) == 0) {
    return false;
  }
  const auto append = [](std::vector<std::string>& into, std::vector<std::string>& from) {
    into.insert(into.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
  };
  append(items.links, found.links);
  append(items.outbounds, found.outbounds);
  append(items.skipped, found.skipped);
  append(items.urls, found.urls);
  return true;
}

bool LooksLikeClashYaml(std::string_view text) {
  std::size_t line = 0;
  while (line < text.size()) {
    const std::string_view rest = text.substr(line);
    if (rest.starts_with("proxies:") || rest.starts_with("proxy-providers:")) {
      return true;
    }
    const std::size_t next = rest.find('\n');
    if (next == std::string_view::npos) {
      break;
    }
    line += next + 1;
  }
  return false;
}

bool ImportYaml(std::string_view text, ImportItems& items) {
  Json document;
  try {
    const YAML::Node root = YAML::Load(std::string(text));
    std::size_t budget = kMaxYamlNodes;
    if (!YamlToJson(root, document, 0, budget)) {
      return false;
    }
  } catch (const std::exception&) {
    return false;  // not YAML after all (YAML::Exception), or too big for memory
  }
  return ImportJson(document, items);
}

}  // namespace sovereign::tray::detail
