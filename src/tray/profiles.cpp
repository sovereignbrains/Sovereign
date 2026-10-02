#include "profiles.h"

#include "share_links.h"

#include <algorithm>
#include <array>
#include <format>

namespace sovereign::tray {

namespace {

bool IsIdChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

bool IsProfileId(std::string_view id) {
  if (id.empty() || id.size() > kMaxProfileId || !std::all_of(id.begin(), id.end(), IsIdChar)) {
    return false;
  }
  // Windows' device names can't be folders, with or without a number.
  static constexpr std::array<std::string_view, 6> kDevices = {"con", "prn", "aux", "nul", "com", "lpt"};
  const std::string_view stem = id.size() == 4 && id[3] >= '0' && id[3] <= '9' ? id.substr(0, 3) : id;
  return std::find(kDevices.begin(), kDevices.end(), stem) == kDevices.end();
}

std::string NewProfileId(const std::vector<Profile>& profiles, std::int64_t seed) {
  std::string id = std::format("p{}", seed < 0 ? -seed : seed);
  for (int n = 2; FindProfile(profiles, id) != nullptr; ++n) {
    id = std::format("p{}-{}", seed < 0 ? -seed : seed, n);
  }
  return id;
}

std::string UniqueProfileName(const std::vector<Profile>& profiles, std::string_view base) {
  const auto taken = [&](const std::string& name) {
    return std::any_of(profiles.begin(), profiles.end(), [&](const Profile& p) { return p.name == name; });
  };
  std::string name(base);
  for (int n = 2; taken(name); ++n) {
    name = std::format("{} {}", base, n);
  }
  return name;
}

std::string CleanProfileName(std::string_view name) {
  std::string out;
  std::size_t characters = 0;
  for (const char c : Trim(name)) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      continue;
    }
    const bool starts = (byte & 0xC0) != 0x80;  // not a continuation byte
    if (starts && ++characters > kMaxProfileName) {
      break;
    }
    out.push_back(c);
  }
  return std::string(Trim(out));
}

std::optional<std::string> ParseProfileTitle(std::string_view header) {
  header = Trim(header);
  std::string text(header);
  if (header.size() >= 7) {
    std::string prefix(header.substr(0, 7));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    if (prefix == "base64:") {
      const auto decoded = DecodeBase64(header.substr(7));
      if (!decoded) {
        return std::nullopt;
      }
      text = *decoded;
    }
  }
  std::string clean = CleanProfileName(text);
  if (clean.empty()) {
    return std::nullopt;
  }
  return clean;
}

std::string DefaultProfileName(std::string_view url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string_view::npos) {
    return "Мой конфиг";
  }
  std::string_view host = url.substr(scheme + 3);
  host = host.substr(0, host.find_first_of("/?#"));
  if (const std::size_t at = host.rfind('@'); at != std::string_view::npos) {
    host.remove_prefix(at + 1);
  }
  std::string name = CleanProfileName(host);
  return name.empty() ? std::string("Подписка") : name;
}

const Profile* FindProfile(const std::vector<Profile>& profiles, std::string_view id) {
  const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const Profile& p) { return p.id == id; });
  return it == profiles.end() ? nullptr : &*it;
}

Profile* FindProfile(std::vector<Profile>& profiles, std::string_view id) {
  const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const Profile& p) { return p.id == id; });
  return it == profiles.end() ? nullptr : &*it;
}

const Profile* FindProfileByUrl(const std::vector<Profile>& profiles, std::string_view url) {
  if (url.empty()) {
    return nullptr;
  }
  const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const Profile& p) { return p.url == url; });
  return it == profiles.end() ? nullptr : &*it;
}

void SetServerEnabled(Profile& profile, const std::string& tag, bool enabled) {
  std::erase(profile.disabled, tag);
  if (!enabled) {
    profile.disabled.push_back(tag);
  }
}

bool IsServerEnabled(const Profile& profile, std::string_view tag) {
  return std::find(profile.disabled.begin(), profile.disabled.end(), tag) == profile.disabled.end();
}

bool IsHwid(std::string_view hwid) {
  return hwid.size() == 32 && std::all_of(hwid.begin(), hwid.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::vector<std::string> SubscriptionHosts(const std::vector<Profile>& profiles) {
  std::vector<std::string> hosts;
  for (const Profile& p : profiles) {
    const std::string_view url = p.url;
    const std::size_t scheme = url.find("://");
    if (scheme == std::string_view::npos) {
      continue;
    }
    std::string_view host = url.substr(scheme + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    if (const std::size_t at = host.rfind('@'); at != std::string_view::npos) {
      host.remove_prefix(at + 1);
    }
    if (host.starts_with('[')) {  // an IPv6 literal: [::1]:443
      const std::size_t close = host.find(']');
      host = close == std::string_view::npos ? std::string_view() : host.substr(1, close - 1);
    } else {
      host = host.substr(0, host.find(':'));
    }
    std::string name(host);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    if (!name.empty() && std::find(hosts.begin(), hosts.end(), name) == hosts.end()) {
      hosts.push_back(std::move(name));
    }
  }
  return hosts;
}

nlohmann::json ProfilesToJson(const std::vector<Profile>& profiles) {
  nlohmann::json list = nlohmann::json::array();
  for (const Profile& p : profiles) {
    list.push_back({{"id", p.id},
                    {"name", p.name},
                    {"userNamed", p.userNamed},
                    {"url", p.url},
                    {"enabled", p.enabled},
                    {"lastRefresh", p.lastRefresh},
                    {"updateHours", p.updateHours},
                    {"autoUpdate", p.autoUpdate},
                    {"userHours", p.userHours},
                    {"disabled", p.disabled},
                    {"hwid", p.hwid}});
  }
  return list;
}

std::vector<Profile> ProfilesFromJson(const nlohmann::json& json, std::string_view legacyActive) {
  std::vector<Profile> profiles;
  if (!json.is_array()) {
    return profiles;
  }
  const auto text = [](const nlohmann::json& entry, const char* key) -> std::optional<std::string> {
    const auto it = entry.find(key);
    if (it == entry.end()) {
      return std::string();
    }
    return it->is_string() ? std::optional<std::string>(it->get<std::string>()) : std::nullopt;
  };
  const auto flag = [](const nlohmann::json& entry, const char* key, bool fallback) {
    const auto it = entry.find(key);
    return it != entry.end() && it->is_boolean() ? it->get<bool>() : fallback;
  };
  const auto number = [](const nlohmann::json& entry, const char* key) -> std::optional<std::int64_t> {
    const auto it = entry.find(key);
    return it != entry.end() && it->is_number_integer() ? std::optional(it->get<std::int64_t>()) : std::nullopt;
  };
  for (const auto& entry : json) {
    if (!entry.is_object()) {
      continue;
    }
    const auto id = text(entry, "id");
    const auto name = text(entry, "name");
    const auto url = text(entry, "url");
    if (!id || !name || !url || !IsProfileId(*id) || FindProfile(profiles, *id) != nullptr) {
      continue;
    }
    Profile p;
    p.id = *id;
    p.name = CleanProfileName(*name);
    p.url = *url;
    p.enabled = flag(entry, "enabled", legacyActive.empty() || *id == legacyActive);
    p.userNamed = flag(entry, "userNamed", false);
    p.lastRefresh = number(entry, "lastRefresh").value_or(0);
    if (const auto hours = number(entry, "updateHours")) {
      p.updateHours = static_cast<int>(std::clamp<std::int64_t>(*hours, 1, 24 * std::int64_t{7}));
    }
    p.autoUpdate = flag(entry, "autoUpdate", true);
    if (const auto hours = number(entry, "userHours")) {
      p.userHours = static_cast<int>(std::clamp<std::int64_t>(*hours, 0, kMaxRefreshHours));
    }
    if (const auto it = entry.find("disabled"); it != entry.end() && it->is_array()) {
      for (const auto& tag : *it) {
        if (tag.is_string() && IsServerEnabled(p, tag.get<std::string>())) {  // each once
          p.disabled.push_back(tag.get<std::string>());
        }
      }
    }
    if (const auto hwid = text(entry, "hwid"); hwid && IsHwid(*hwid)) {
      p.hwid = *hwid;  // anything else: a new one at the next fetch
    }
    if (p.name.empty()) {
      p.name = UniqueProfileName(profiles, "Конфиг");
    }
    profiles.push_back(std::move(p));
  }
  return profiles;
}
}  // namespace sovereign::tray
