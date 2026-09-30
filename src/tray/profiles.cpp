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

std::string ActiveAfterRemoval(const std::vector<Profile>& profiles, std::string_view active,
                               std::string_view removed) {
  if (active != removed && FindProfile(profiles, active) != nullptr) {
    return std::string(active);
  }
  const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const Profile& p) { return p.id == removed; });
  if (it == profiles.end()) {
    return profiles.empty() ? std::string() : profiles.front().id;
  }
  if (it + 1 != profiles.end()) {
    return (it + 1)->id;
  }
  return it != profiles.begin() ? (it - 1)->id : std::string();
}

nlohmann::json ProfilesToJson(const std::vector<Profile>& profiles) {
  nlohmann::json list = nlohmann::json::array();
  for (const Profile& p : profiles) {
    list.push_back({{"id", p.id},
                    {"name", p.name},
                    {"url", p.url},
                    {"lastRefresh", p.lastRefresh},
                    {"updateHours", p.updateHours},
                    {"protocol", p.protocol}});
  }
  return list;
}

std::vector<Profile> ProfilesFromJson(const nlohmann::json& json) {
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
  for (const auto& entry : json) {
    if (!entry.is_object()) {
      continue;
    }
    const auto id = text(entry, "id");
    const auto name = text(entry, "name");
    const auto url = text(entry, "url");
    const auto protocol = text(entry, "protocol");
    if (!id || !name || !url || !protocol || !IsProfileId(*id) || FindProfile(profiles, *id) != nullptr) {
      continue;
    }
    Profile p;
    p.id = *id;
    p.name = CleanProfileName(*name);
    p.url = *url;
    p.protocol = *protocol;
    if (const auto it = entry.find("lastRefresh"); it != entry.end() && it->is_number_integer()) {
      p.lastRefresh = it->get<std::int64_t>();
    }
    if (const auto it = entry.find("updateHours"); it != entry.end() && it->is_number_integer()) {
      p.updateHours = static_cast<int>(std::clamp<std::int64_t>(it->get<std::int64_t>(), 1, std::int64_t{24} * 7));
    }
    if (p.name.empty()) {
      p.name = UniqueProfileName(profiles, "Конфиг");
    }
    profiles.push_back(std::move(p));
  }
  return profiles;
}

}  // namespace sovereign::tray
