#include "warp.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "share_links.h"

namespace sovereign::tray {

namespace {

using Json = nlohmann::json;

std::string Text(const Json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

const Json& Object(const Json& object, const char* key) {
  static const Json kNone = Json::object();
  const auto it = object.find(key);
  return it != object.end() && it->is_object() ? *it : kNone;
}

// 32 bytes in base64: a WireGuard key.
bool IsKey(std::string_view base64) {
  const auto bytes = DecodeBase64(base64);
  return bytes && bytes->size() == 32;
}

// An address in the API's answer, without a prefix: digits, hex, dots, colons.
bool IsAddress(std::string_view text) {
  return !text.empty() && text.size() <= 45 && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':';
  });
}

// "engage.cloudflareclient.com:2408" -> host and port.
bool SplitHostPort(std::string_view text, std::string& host, int& port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || text.front() == '[') {
    return false;
  }
  int value = 0;
  for (const char c : text.substr(colon + 1)) {
    if (c < '0' || c > '9' || value > 65535) {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  if (value <= 0 || value > 65535) {
    return false;
  }
  host = std::string(text.substr(0, colon));
  port = value;
  return true;
}

}  // namespace

std::string WarpRegisterBody(std::string_view publicKey, std::string_view tos) {
  return Json{{"key", publicKey},       {"install_id", ""}, {"fcm_token", ""}, {"tos", tos},
              {"model", "PC"},          {"serial_number", ""}, {"locale", "en_US"}}
      .dump();
}

std::optional<WarpAccount> ParseWarpRegistration(std::string_view response, std::string privateKey) {
  const Json json = Json::parse(response, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object() || !IsKey(privateKey)) {
    return std::nullopt;
  }
  // The answer is the device, its config inside: {"config": {"peers": [...],
  // "interface": {"addresses": {...}}, "client_id": "..."}} - in some API
  // versions under "result".
  const Json& root = json.contains("result") && json["result"].is_object() ? json["result"] : json;
  const Json& config = Object(root, "config");
  WarpAccount account;
  account.privateKey = std::move(privateKey);
  account.id = Text(root, "id");
  account.token = Text(root, "token");
  const Json& addresses = Object(Object(config, "interface"), "addresses");
  account.address4 = Text(addresses, "v4");
  account.address6 = Text(addresses, "v6");
  const auto peers = config.find("peers");
  if (peers == config.end() || !peers->is_array() || peers->empty() || !(*peers)[0].is_object()) {
    return std::nullopt;
  }
  const Json& peer = (*peers)[0];
  account.peerKey = Text(peer, "public_key");
  if (const std::string host = Text(Object(peer, "endpoint"), "host"); !host.empty()) {
    if (!SplitHostPort(host, account.host, account.port)) {
      return std::nullopt;
    }
  }
  // client_id: 3 bytes, base64 - WireGuard's "reserved" field.
  if (const auto id = DecodeBase64(Text(config, "client_id")); id && id->size() == 3) {
    for (std::size_t i = 0; i < 3; ++i) {
      account.reserved[i] = static_cast<unsigned char>((*id)[i]);
    }
  }
  if (!IsAddress(account.address4) || (!account.address6.empty() && !IsAddress(account.address6)) ||
      !IsKey(account.peerKey)) {
    return std::nullopt;
  }
  return account;
}

nlohmann::json WarpToJson(const WarpAccount& account) {
  return Json{{"privateKey", account.privateKey}, {"id", account.id},           {"token", account.token},
              {"address4", account.address4},     {"address6", account.address6}, {"peerKey", account.peerKey},
              {"host", account.host},             {"port", account.port},         {"reserved", account.reserved}};
}

std::optional<WarpAccount> WarpFromJson(const nlohmann::json& json) {
  if (!json.is_object()) {
    return std::nullopt;
  }
  WarpAccount account;
  account.privateKey = Text(json, "privateKey");
  account.id = Text(json, "id");
  account.token = Text(json, "token");
  account.address4 = Text(json, "address4");
  account.address6 = Text(json, "address6");
  account.peerKey = Text(json, "peerKey");
  if (const std::string host = Text(json, "host"); !host.empty()) {
    account.host = host;
  }
  if (const auto port = json.find("port"); port != json.end() && port->is_number_integer()) {
    account.port = std::clamp(port->get<int>(), 1, 65535);
  }
  if (const auto reserved = json.find("reserved"); reserved != json.end() && reserved->is_array() &&
                                                   reserved->size() == 3) {
    for (std::size_t i = 0; i < 3; ++i) {
      const Json& value = (*reserved)[i];
      account.reserved[i] = value.is_number_integer() ? std::clamp(value.get<int>(), 0, 255) : 0;
    }
  }
  const bool hostOk = !account.host.empty() && account.host.size() <= 253 &&
                      std::all_of(account.host.begin(), account.host.end(), [](char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                               c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
                      });
  if (!IsKey(account.privateKey) || !IsKey(account.peerKey) || !IsAddress(account.address4) ||
      (!account.address6.empty() && !IsAddress(account.address6)) || !hostOk) {
    return std::nullopt;
  }
  return account;
}

nlohmann::ordered_json WarpEndpoint(const WarpAccount& account, std::string_view tag, std::string_view detour) {
  nlohmann::ordered_json addresses = nlohmann::ordered_json::array({account.address4 + "/32"});
  if (!account.address6.empty()) {
    addresses.push_back(account.address6 + "/128");
  }
  nlohmann::ordered_json endpoint = {
      {"type", "wireguard"},
      {"tag", tag},
      {"mtu", 1280},
      {"address", std::move(addresses)},
      {"private_key", account.privateKey},
      {"peers", nlohmann::ordered_json::array({nlohmann::ordered_json{
                    {"address", account.host},
                    {"port", account.port},
                    {"public_key", account.peerKey},
                    {"allowed_ips", nlohmann::ordered_json::array({"0.0.0.0/0", "::/0"})},
                    {"reserved", account.reserved}}})}};
  if (!detour.empty()) {
    endpoint["detour"] = detour;
  }
  return endpoint;
}

}  // namespace sovereign::tray
