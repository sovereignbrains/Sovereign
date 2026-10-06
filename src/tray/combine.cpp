#include "combine.h"

#include "config_sync.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <map>
#include <set>
#include <utility>

namespace sovereign::tray {

namespace {

using Json = nlohmann::ordered_json;

constexpr std::array<const char*, 2> kLists = {"outbounds", "endpoints"};

std::string Str(const Json& object, const char* key) {
  if (!object.is_object()) {
    return {};
  }
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string Tag(const Json& o) { return Str(o, "tag"); }

bool IsGroup(const Json& o) {
  const std::string type = Str(o, "type");
  return type == "selector" || type == "urltest";
}

bool IsServer(const Json& o) {
  const std::string type = Str(o, "type");
  return !type.empty() && type != "selector" && type != "urltest" && type != "direct" && type != "block" &&
         type != "dns";
}

Json* List(Json& config, const char* name) {
  if (!config.is_object()) {
    return nullptr;
  }
  const auto it = config.find(name);
  return it != config.end() && it->is_array() ? &*it : nullptr;
}

std::vector<std::string> Members(const Json& group) {
  std::vector<std::string> members;
  if (const auto it = group.find("outbounds"); it != group.end() && it->is_array()) {
    for (const Json& m : *it) {
      if (m.is_string()) {
        members.push_back(m.get<std::string>());
      }
    }
  }
  return members;
}

// Takes `removed`'s outbounds and endpoints out, and with them whatever can't
// work without them: outbounds chained through one (detour) and groups left
// empty. `removed` grows by those.
void Remove(Json& config, std::set<std::string>& removed) {
  for (bool changed = true; changed;) {
    changed = false;
    for (const char* name : kLists) {
      Json* list = List(config, name);
      if (list == nullptr) {
        continue;
      }
      for (Json& o : *list) {
        const std::string tag = Tag(o);
        if (tag.empty() || removed.contains(tag)) {
          continue;
        }
        if (removed.contains(Str(o, "detour"))) {
          removed.insert(tag);
          changed = true;
          continue;
        }
        if (IsGroup(o) && o.contains("outbounds") && o["outbounds"].is_array()) {
          Json& members = o["outbounds"];
          const std::size_t before = members.size();
          Json kept = Json::array();
          for (const Json& m : members) {
            if (!m.is_string() || !removed.contains(m.get<std::string>())) {
              kept.push_back(m);
            }
          }
          if (kept.size() != before) {
            members = std::move(kept);
          }
          if (members.empty()) {
            removed.insert(tag);
            changed = true;
          } else if (removed.contains(Str(o, "default"))) {
            o.erase("default");
          }
        }
      }
    }
  }
  for (const char* name : kLists) {
    if (Json* list = List(config, name)) {
      Json kept = Json::array();
      for (Json& o : *list) {
        if (!removed.contains(Tag(o))) {
          kept.push_back(std::move(o));
        }
      }
      *list = std::move(kept);
    }
  }
}

// References to removed outbounds outside the outbound lists - route.final,
// route rules, DNS servers' detours, rule sets' download_detour, inbounds' -
// to `replacement`; a route rule pointing at one is dropped if there's none.
void Repoint(Json& node, const std::set<std::string>& removed, const std::string& replacement, int depth) {
  if (depth > kMaxConfigDepth) {
    return;
  }
  if (node.is_array()) {
    Json kept = Json::array();
    for (Json& item : node) {
      if (replacement.empty() && item.is_object() && removed.contains(Str(item, "outbound"))) {
        continue;  // a rule with nowhere to send its traffic
      }
      Repoint(item, removed, replacement, depth + 1);
      kept.push_back(std::move(item));
    }
    node = std::move(kept);
    return;
  }
  if (!node.is_object()) {
    return;
  }
  for (const char* key : {"outbound", "detour", "download_detour", "final"}) {
    if (const std::string target = Str(node, key); !target.empty() && removed.contains(target)) {
      if (replacement.empty()) {
        node.erase(key);
      } else {
        node[key] = replacement;
      }
    }
  }
  for (auto it = node.begin(); it != node.end(); ++it) {
    if (depth == 0 && (it.key() == "outbounds" || it.key() == "endpoints")) {
      continue;  // done by Remove
    }
    Repoint(it.value(), removed, replacement, depth + 1);
  }
}

// The selector the traffic goes through: the one route.final names, else
// the first; its index in outbounds, or npos.
std::size_t MainSelector(Json& config) {
  Json* outbounds = List(config, "outbounds");
  if (outbounds == nullptr) {
    return std::string::npos;
  }
  const std::string final = config.contains("route") ? Str(config["route"], "final") : std::string();
  std::size_t first = std::string::npos;
  for (std::size_t i = 0; i < outbounds->size(); ++i) {
    const Json& o = (*outbounds)[i];
    if (Str(o, "type") != "selector") {
      continue;
    }
    if (!final.empty() && Tag(o) == final) {
      return i;
    }
    if (first == std::string::npos) {
      first = i;
    }
  }
  return first;
}

std::set<std::string> AllTags(Json& config) {
  std::set<std::string> tags;
  for (const char* name : kLists) {
    if (Json* list = List(config, name)) {
      for (const Json& o : *list) {
        if (const std::string tag = Tag(o); !tag.empty()) {
          tags.insert(tag);
        }
      }
    }
  }
  return tags;
}

std::vector<std::string> ServerTags(Json& config) {
  std::vector<std::string> tags;
  for (const char* name : kLists) {
    if (Json* list = List(config, name)) {
      for (const Json& o : *list) {
        if (IsServer(o) && !Tag(o).empty()) {
          tags.push_back(Tag(o));
        }
      }
    }
  }
  return tags;
}

std::set<std::string> DnsServerTags(const Json& config) {
  std::set<std::string> tags;
  if (config.contains("dns") && config["dns"].is_object() && config["dns"].contains("servers") &&
      config["dns"]["servers"].is_array()) {
    for (const Json& s : config["dns"]["servers"]) {
      if (const std::string tag = Tag(s); !tag.empty()) {
        tags.insert(tag);
      }
    }
  }
  return tags;
}

std::optional<Json> Parse(const ProfileConfig& part, std::string& error) {
  if (NestingDepth(part.config) > kMaxConfigDepth) {
    error = part.name + ": конфиг слишком глубоко вложен";
    return std::nullopt;
  }
  Json json = Json::parse(part.config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    error = part.name + ": config.json - не JSON-объект";
    return std::nullopt;
  }
  return json;
}

// A part's servers, ready to go into the frame: renamed where their tag is
// taken, chains within the part kept, what the frame can't resolve dropped.
struct Moved {
  Json outbounds = Json::array();
  Json endpoints = Json::array();
  std::vector<std::string> tags;
};

Moved TakeServers(Json& part, const std::string& partName, std::set<std::string>& taken,
                  const std::set<std::string>& frameDns) {
  std::map<std::string, std::string> renamed;  // the part's tag -> its tag in the frame
  std::vector<std::pair<const char*, Json>> servers;
  for (const char* name : kLists) {
    if (Json* list = List(part, name)) {
      for (Json& o : *list) {
        const std::string tag = Tag(o);
        if (!IsServer(o) || tag.empty()) {
          continue;
        }
        std::string fresh = tag;
        if (taken.contains(fresh)) {
          // "NL · <configuration>"; a configuration named after its only
          // server (a key's) would say the name twice - "AnyTLS-REALITY ·
          // AnyTLS-REALITY" - so then it's numbered: "AnyTLS-REALITY 2".
          const bool same = partName.empty() || partName == tag;
          fresh = same ? std::format("{} 2", tag) : std::format("{} · {}", tag, partName);
          for (int n = same ? 3 : 2; taken.contains(fresh); ++n) {
            fresh = same ? std::format("{} {}", tag, n) : std::format("{} · {} {}", tag, partName, n);
          }
        }
        taken.insert(fresh);
        renamed[tag] = fresh;
        servers.emplace_back(name, std::move(o));
      }
    }
  }
  // Chains: through another server of the part (renamed with it), or dropped.
  std::set<std::string> dropped;
  for (bool changed = true; changed;) {
    changed = false;
    for (auto& [list, o] : servers) {
      const std::string tag = Tag(o);
      const std::string detour = Str(o, "detour");
      if (dropped.contains(tag) || detour.empty()) {
        continue;
      }
      if (!renamed.contains(detour) || dropped.contains(detour)) {
        dropped.insert(tag);
        changed = true;
      }
    }
  }
  Moved moved;
  for (auto& [list, o] : servers) {
    const std::string tag = Tag(o);
    if (dropped.contains(tag)) {
      taken.erase(renamed[tag]);
      continue;
    }
    o["tag"] = renamed[tag];
    if (const std::string detour = Str(o, "detour"); !detour.empty()) {
      o["detour"] = renamed[detour];
    }
    // Its DNS server names are the part's: the frame's own resolver serves it
    // unless the frame has one by that name.
    if (o.contains("domain_resolver")) {
      const Json& resolver = o["domain_resolver"];
      const std::string server = resolver.is_string() ? resolver.get<std::string>() : Str(resolver, "server");
      if (!frameDns.contains(server)) {
        o.erase("domain_resolver");
      }
    }
    moved.tags.push_back(renamed[tag]);
    (std::string_view(list) == "endpoints" ? moved.endpoints : moved.outbounds).push_back(std::move(o));
  }
  return moved;
}

// Chains (ProfileConfig::via): each part connecting through another gets
// that part's servers as a URL test of their own, and its own servers -
// those not already chained within it - that test as their detour. A chain
// that can't be made drops the part's servers: never directly instead.
// `tags[i]`: part i's servers in the frame.
void MakeChains(Json& frame, const std::vector<ProfileConfig>& parts, const std::vector<std::vector<std::string>>& tags,
                const std::string& replacement, std::vector<std::string>& notes) {
  std::map<std::string, std::size_t> byId;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (!parts[i].id.empty()) {
      byId.emplace(parts[i].id, i);
    }
  }
  std::set<std::string> taken = AllTags(frame);
  std::map<std::size_t, std::string> groups;  // a part chained through -> its URL test's tag
  std::set<std::string> dropped;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (parts[i].via.empty() || tags[i].empty()) {
      continue;
    }
    const auto target = byId.find(parts[i].via);
    if (target == byId.end() || target->second == i || !parts[target->second].via.empty() ||
        tags[target->second].empty()) {
      dropped.insert(tags[i].begin(), tags[i].end());
      notes.push_back(parts[i].name +
                      ": подключается через другую конфигурацию, а та выключена, пуста или сама идёт цепочкой — "
                      "серверы пропущены, чтобы не подключаться напрямую");
      continue;
    }
    std::string& group = groups[target->second];
    if (group.empty()) {
      const std::string& name = parts[target->second].name;
      group = std::format("«{}»", name);
      for (int n = 2; taken.contains(group); ++n) {
        group = std::format("«{}» {}", name, n);
      }
      taken.insert(group);
      if (Json* outbounds = List(frame, "outbounds")) {
        outbounds->push_back(Json{{"type", "urltest"}, {"tag", group}, {"outbounds", tags[target->second]}});
      }
    }
    const std::set<std::string> mine(tags[i].begin(), tags[i].end());
    for (const char* name : kLists) {
      if (Json* list = List(frame, name)) {
        for (Json& o : *list) {
          if (mine.contains(Tag(o)) && Str(o, "detour").empty()) {
            o["detour"] = group;
          }
        }
      }
    }
  }
  if (!dropped.empty()) {
    Remove(frame, dropped);
    Repoint(frame, dropped, replacement, 0);
  }
}

std::string Label(const Json& o) {
  static const std::map<std::string, std::string, std::less<>> kNames = {
      {"vless", "VLESS"},         {"vmess", "VMess"},       {"trojan", "Trojan"},   {"shadowsocks", "Shadowsocks"},
      {"hysteria2", "Hysteria2"}, {"hysteria", "Hysteria"}, {"tuic", "TUIC"},       {"anytls", "AnyTLS"},
      {"wireguard", "WireGuard"}, {"naive", "Naive"},       {"socks", "SOCKS"},     {"http", "HTTP"},
      {"ssh", "SSH"},             {"shadowtls", "ShadowTLS"}, {"snell", "Snell"}, {"tor", "Tor"},
      {"tailscale", "Tailscale"}};
  const std::string type = Str(o, "type");
  const auto known = kNames.find(type);
  std::string label = known != kNames.end() ? known->second : type;
  if (o.contains("tls") && o["tls"].is_object()) {
    const Json& tls = o["tls"];
    if (tls.contains("reality") && tls["reality"].is_object() && tls["reality"].contains("enabled") &&
        tls["reality"]["enabled"] == true) {
      label += " · REALITY";
    }
  }
  if (o.contains("transport") && o["transport"].is_object()) {
    std::string transport = Str(o["transport"], "type");
    std::transform(transport.begin(), transport.end(), transport.begin(),
                   [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
    if (!transport.empty()) {
      label += " · " + transport;
    }
  }
  if (!Str(o, "detour").empty()) {
    label += " · через " + Str(o, "detour");
  }
  return label;
}

}  // namespace

bool IsRenamedTag(std::string_view combined, std::string_view tag, std::string_view partName) {
  if (combined == tag) {
    return true;
  }
  // A number at the end: " N", N >= 2.
  const auto numbered = [](std::string_view rest) {
    return rest.size() >= 2 && rest.front() == ' ' &&
           std::all_of(rest.begin() + 1, rest.end(), [](char c) { return c >= '0' && c <= '9'; }) && rest != " 0" &&
           rest != " 1";
  };
  if (!combined.starts_with(tag)) {
    return false;
  }
  std::string_view rest = combined.substr(tag.size());
  if (partName.empty() || partName == tag) {
    return numbered(rest);
  }
  const std::string named = std::format(" · {}", partName);
  if (!rest.starts_with(named)) {
    return false;
  }
  rest.remove_prefix(named.size());
  return rest.empty() || numbered(rest);
}

CombinedConfig CombineConfigs(const std::vector<ProfileConfig>& parts, const std::optional<std::string>& ownFrame) {
  CombinedConfig result;
  if (parts.empty()) {
    result.error = "ни одна конфигурация не включена";
    return result;
  }
  if (!ownFrame && parts.size() == 1 && parts[0].disabled.empty() && parts[0].via.empty()) {
    result.config = parts[0].config;
    return result;
  }
  // The frame: the client's own, every part giving servers - or the first
  // part's, less its servers switched off.
  std::size_t first = 0;
  std::set<std::string> removed;
  std::optional<Json> frame;
  if (ownFrame) {
    frame = Parse({.name = "Sovereign", .config = *ownFrame, .disabled = {}, .id = {}, .via = {}}, result.error);
  } else {
    frame = Parse(parts[0], result.error);
    first = 1;
    removed.insert(parts[0].disabled.begin(), parts[0].disabled.end());
  }
  if (!frame) {
    return result;
  }
  if (!ownFrame) {
    Remove(*frame, removed);  // the own frame's groups are empty until the servers come
  }

  // Servers from the others go into a selector: the frame's, or one made for
  // them around what the frame's route went to.
  std::size_t selector = MainSelector(*frame);
  const bool adding = parts.size() > first;
  if (selector == std::string::npos && adding) {
    std::set<std::string> taken = AllTags(*frame);
    std::string tag = "proxy";
    for (int n = 2; taken.contains(tag); ++n) {
      tag = std::format("proxy {}", n);
    }
    Json made = {{"type", "selector"}, {"tag", tag}, {"outbounds", ServerTags(*frame)}};
    // Every key the frame needs first: an ordered_json object keeps its
    // values in a vector, so adding a key moves the others - references
    // are taken only after the last one is added (found by fuzz-tray-input).
    if (!frame->contains("route") || !(*frame)["route"].is_object()) {
      (*frame)["route"] = Json::object();
    }
    if (!frame->contains("outbounds") || !(*frame)["outbounds"].is_array()) {
      (*frame)["outbounds"] = Json::array();
    }
    Json& route = (*frame)["route"];
    Json& outbounds = (*frame)["outbounds"];
    if (const std::string final = Str(route, "final"); !final.empty() && !removed.contains(final)) {
      made["default"] = final;
    }
    outbounds.insert(outbounds.begin(), std::move(made));
    route["final"] = tag;
    selector = 0;
  }
  std::string replacement;
  if (selector != std::string::npos) {
    replacement = Tag((*frame)["outbounds"][selector]);
  } else if (const auto servers = ServerTags(*frame); !servers.empty()) {
    replacement = servers.front();
  }
  Repoint(*frame, removed, replacement, 0);

  std::set<std::string> taken = AllTags(*frame);
  const std::set<std::string> frameDns = DnsServerTags(*frame);
  std::vector<std::vector<std::string>> partTags(parts.size());  // each part's servers in the frame (chains)
  if (first == 1) {
    partTags[0] = ServerTags(*frame);
  }
  for (std::size_t i = first; i < parts.size(); ++i) {
    std::string error;
    auto part = Parse(parts[i], error);
    if (!part) {
      result.notes.push_back(error);
      continue;
    }
    std::set<std::string> off(parts[i].disabled.begin(), parts[i].disabled.end());
    Remove(*part, off);
    Moved moved = TakeServers(*part, parts[i].name, taken, frameDns);
    partTags[i] = moved.tags;
    // A key added to the frame moves its other values: endpoints first, the
    // reference to outbounds after.
    if (!moved.endpoints.empty() && (!frame->contains("endpoints") || !(*frame)["endpoints"].is_array())) {
      (*frame)["endpoints"] = Json::array();
    }
    for (Json& e : moved.endpoints) {
      (*frame)["endpoints"].push_back(std::move(e));
    }
    Json& outbounds = (*frame)["outbounds"];
    for (Json& o : moved.outbounds) {
      outbounds.push_back(std::move(o));
    }
    // Into the selector, and into the URL tests it offers ("auto").
    Json& main = outbounds[selector];
    const std::vector<std::string> offered = Members(main);
    for (const std::string& tag : moved.tags) {
      main["outbounds"].push_back(tag);
    }
    for (Json& o : outbounds) {
      if (Str(o, "type") == "urltest" &&
          std::find(offered.begin(), offered.end(), Tag(o)) != offered.end()) {
        for (const std::string& tag : moved.tags) {
          o["outbounds"].push_back(tag);
        }
      }
    }
  }

  MakeChains(*frame, parts, partTags, replacement, result.notes);

  if (ServerTags(*frame).empty()) {
    result.error = "все серверы выключены";
    return result;
  }
  if (selector != std::string::npos && Members((*frame)["outbounds"][selector]).empty()) {
    result.error = "в общем списке не осталось серверов";
    return result;
  }
  result.config = frame->dump(2, ' ', false, Json::error_handler_t::replace);
  return result;
}

std::vector<ServerInfo> ListServers(std::string_view config) {
  std::vector<ServerInfo> servers;
  if (NestingDepth(config) > kMaxConfigDepth) {
    return servers;
  }
  Json json = Json::parse(config, nullptr, /*allow_exceptions=*/false);
  for (const char* name : kLists) {
    if (Json* list = List(json, name)) {
      for (const Json& o : *list) {
        if (IsServer(o) && !Tag(o).empty()) {
          servers.push_back({Tag(o), Str(o, "type"), Label(o)});
        }
      }
    }
  }
  return servers;
}

}  // namespace sovereign::tray
