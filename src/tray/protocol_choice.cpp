#include "protocol_choice.h"

#include "json_field.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <utility>

namespace sovereign::tray {

namespace {

using Json = nlohmann::json;

bool IsSelector(const Json& o) { return Field<std::string>(o, "type", {}) == "selector"; }

// The selector to use: the one route.final names, else the first one.
Json* FindSelector(Json& config) {
  if (!config.is_object() || !config.contains("outbounds") || !config["outbounds"].is_array()) {
    return nullptr;
  }
  Json& outbounds = config["outbounds"];
  std::string final;
  if (config.contains("route") && config["route"].is_object() && config["route"].contains("final") &&
      config["route"]["final"].is_string()) {
    final = config["route"]["final"].get<std::string>();
  }
  Json* first = nullptr;
  for (Json& o : outbounds) {
    if (!IsSelector(o)) {
      continue;
    }
    if (!final.empty() && Field<std::string>(o, "tag", {}) == final) {
      return &o;
    }
    if (first == nullptr) {
      first = &o;
    }
  }
  return first;
}

}  // namespace

ProtocolChoices FindProtocolChoices(std::string_view config) {
  Json json = Json::parse(config, nullptr, /*allow_exceptions=*/false);
  const Json* selector = FindSelector(json);
  ProtocolChoices choices;
  if (selector == nullptr) {
    return choices;
  }
  choices.selector = Field<std::string>(*selector, "tag", {});
  if (selector->contains("outbounds") && (*selector)["outbounds"].is_array()) {
    for (const Json& o : (*selector)["outbounds"]) {
      if (o.is_string()) {
        choices.options.push_back(o.get<std::string>());
      }
    }
  }
  choices.configDefault = Field<std::string>(*selector, "default", choices.options.empty() ? std::string{} : choices.options[0]);
  const auto isOption = [&](const std::string& tag) {
    return std::find(choices.options.begin(), choices.options.end(), tag) != choices.options.end();
  };
  for (const Json& o : json["outbounds"]) {
    const std::string tag = Field<std::string>(o, "tag", {});
    if (Field<std::string>(o, "type", {}) != "urltest" || !isOption(tag) || !o.contains("outbounds") ||
        !o["outbounds"].is_array()) {
      continue;
    }
    std::vector<std::string>& servers = choices.groups[tag];
    for (const Json& server : o["outbounds"]) {
      if (server.is_string() && isOption(server.get<std::string>())) {
        servers.push_back(server.get<std::string>());
      }
    }
  }
  return choices;
}

namespace {

// A protocol's name as its authors write it; empty for what isn't a server.
std::string_view ProtocolName(std::string_view type) {
  static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
      {"anytls", "AnyTLS"},       {"vless", "VLESS"},       {"vmess", "VMess"},
      {"trojan", "Trojan"},       {"shadowsocks", "Shadowsocks"}, {"shadowsocksr", "ShadowsocksR"},
      {"hysteria", "Hysteria"},   {"hysteria2", "Hysteria 2"},    {"tuic", "TUIC"},
      {"wireguard", "WireGuard"}, {"naive", "NaïveProxy"},        {"shadowtls", "ShadowTLS"},
      {"ssh", "SSH"},             {"http", "HTTP"},               {"socks", "SOCKS"},
      {"tor", "Tor"},
  };
  for (const auto& [t, name] : kNames) {
    if (t == type) {
      return name;
    }
  }
  return {};
}

// The ones that are TLS themselves: "+ TLS" would say nothing.
bool TlsOfItsOwn(std::string_view type) {
  return type == "anytls" || type == "trojan" || type == "hysteria" || type == "hysteria2" || type == "tuic" ||
         type == "naive" || type == "shadowtls";
}

std::string_view TransportName(std::string_view type) {
  return type == "ws"            ? "WebSocket"
         : type == "grpc"        ? "gRPC"
         : type == "http"        ? "HTTP/2"
         : type == "httpupgrade" ? "HTTPUpgrade"
         : type == "quic"        ? "QUIC"
                                 : std::string_view();
}

int Port(std::int64_t port) { return port > 0 && port <= 65535 ? static_cast<int>(port) : 0; }

bool Enabled(const Json& o, const char* key) {
  return o.contains(key) && o[key].is_object() && Field<bool>(o[key], "enabled", false);
}

std::optional<ServerTitle> TitleOf(const Json& o) {
  const auto type = Field<std::string>(o, "type", {});
  if (type == "urltest") {
    return ServerTitle{.title = "Авто", .host = {}, .port = 0};
  }
  std::string name(ProtocolName(type));
  if (name.empty()) {
    return std::nullopt;
  }
  if (type == "shadowsocks" && Field<std::string>(o, "method", {}).starts_with("2022-")) {
    name = "Shadowsocks 2022";
  }
  std::vector<std::string> parts = {name};
  if (Field<std::string>(o, "flow", {}) == "xtls-rprx-vision") {
    parts.emplace_back("Vision");
  }
  if (o.contains("transport") && o["transport"].is_object()) {
    if (const auto transport = TransportName(Field<std::string>(o["transport"], "type", {})); !transport.empty()) {
      parts.emplace_back(transport);
    }
  }
  if (Enabled(o, "tls")) {
    const Json& tls = o["tls"];
    if (Enabled(tls, "reality")) {
      parts.emplace_back("REALITY");
    } else if (Enabled(tls, "ech")) {
      parts.emplace_back("ECH");
    } else if (type == "http") {
      parts.front() = "HTTPS";
    } else if (!TlsOfItsOwn(type)) {
      parts.emplace_back("TLS");
    }
  }
  ServerTitle title;
  for (const std::string& part : parts) {
    title.title += (title.title.empty() ? "" : " + ") + part;
  }
  title.host = Field<std::string>(o, "server", {});
  title.port = Port(Field<std::int64_t>(o, "server_port", 0));
  if (title.host.empty() && o.contains("peers") && o["peers"].is_array() && !o["peers"].empty()) {
    title.host = Field<std::string>(o["peers"][0], "address", {});  // a WireGuard endpoint
    title.port = Port(Field<std::int64_t>(o["peers"][0], "port", 0));
  }
  return title;
}

}  // namespace

bool NameIsTitle(std::string_view name, std::string_view title) {
  const auto letters = [](std::string_view text) {
    std::string out;
    for (const char c : text) {
      const auto u = static_cast<unsigned char>(c);
      if (std::isalnum(u) != 0 || u >= 0x80) {
        out += static_cast<char>(std::tolower(u));
      }
    }
    return out;
  };
  const std::string a = letters(name);
  return !a.empty() && a == letters(title);
}

std::map<std::string, ServerTitle> ServerTitles(std::string_view config) {
  std::map<std::string, ServerTitle> titles;
  const Json c = Json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!c.is_object()) {
    return titles;
  }
  for (const char* list : {"outbounds", "endpoints"}) {
    if (!c.contains(list) || !c[list].is_array()) {
      continue;
    }
    for (const Json& o : c[list]) {
      const auto tag = o.is_object() ? Field<std::string>(o, "tag", {}) : std::string();
      if (tag.empty()) {
        continue;
      }
      if (auto title = TitleOf(o)) {
        titles[tag] = std::move(*title);
      }
    }
  }
  return titles;
}

std::string ApplyProtocolChoice(std::string_view config, const std::string& choice) {
  Json json = Json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    return std::string(config);
  }
  Json* selector = FindSelector(json);
  if (selector != nullptr && !choice.empty() && selector->contains("outbounds") && (*selector)["outbounds"].is_array()) {
    const Json& options = (*selector)["outbounds"];
    if (std::find(options.begin(), options.end(), Json(choice)) != options.end()) {
      (*selector)["default"] = choice;
    }
  }
  return json.dump();
}

}  // namespace sovereign::tray
