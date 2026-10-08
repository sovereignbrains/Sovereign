#include "settings.h"

#include "log_level.h"
#include "relay.h"
#include "share_links.h"

#include <windows.h>
#include <dpapi.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>

namespace sovereign::tray {

namespace {

std::optional<std::string> ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteTextAtomically(const std::filesystem::path& path, const std::string& text) {
  const std::filesystem::path temp = path.wstring() + L".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    THROW_HR_IF(E_FAIL, !out);
    out << text;
    out.close();
    THROW_HR_IF(E_FAIL, !out);
  }
  THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING));
}

}  // namespace

std::filesystem::path DataDir() {
  wchar_t localAppData[MAX_PATH]{};
  const DWORD n = ExpandEnvironmentStringsW(L"%LOCALAPPDATA%", localAppData, MAX_PATH);
  THROW_LAST_ERROR_IF(n == 0 || n > MAX_PATH);
  std::filesystem::path dir = std::filesystem::path(localAppData) / L"Sovereign";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  THROW_HR_IF(HRESULT_FROM_WIN32(ec.value()), ec.operator bool());
  return dir;
}

const std::string* AppPath(const TraySettings& settings, const std::string& app) {
  const auto it = std::find_if(settings.appPaths.begin(), settings.appPaths.end(),
                               [&](const auto& entry) { return entry.first == app; });
  return it == settings.appPaths.end() ? nullptr : &it->second;
}

void SetAppPath(TraySettings& settings, const std::string& app, std::string path) {
  const auto it = std::find_if(settings.appPaths.begin(), settings.appPaths.end(),
                               [&](const auto& entry) { return entry.first == app; });
  if (it != settings.appPaths.end()) {
    it->second = std::move(path);
  } else {
    settings.appPaths.emplace_back(app, std::move(path));
  }
}

namespace {

// A config.json in the folder itself - a tray's one configuration from
// before profiles, or one put there by hand - becomes a profile: its files
// move into the profile's folder. `legacy` is tray.json from before profiles
// (its subscription and choices go with the config), or null.
void AdoptRootConfig(TraySettings& settings, const nlohmann::json& legacy) {
  const std::filesystem::path root = DataDir();
  std::error_code ec;
  const bool hasConfig = std::filesystem::is_regular_file(root / L"config.json", ec);
  const auto field = [&](const char* key) {
    const auto v = legacy.is_object() ? legacy.find(key) : legacy.end();
    return legacy.is_object() && v != legacy.end() ? *v : nlohmann::json();
  };
  const std::string url = field("subscriptionUrl").is_string() ? field("subscriptionUrl").get<std::string>() : "";
  if (!hasConfig && url.empty()) {
    return;
  }
  Profile profile;
  profile.id = legacy.is_object() && FindProfile(settings.profiles, "p1") == nullptr
                   ? std::string("p1")
                   : NewProfileId(settings.profiles, std::time(nullptr));
  profile.url = url;
  profile.name = UniqueProfileName(settings.profiles, DefaultProfileName(url));
  if (field("lastRefresh").is_number_integer()) {
    profile.lastRefresh = field("lastRefresh").get<std::int64_t>();
  }
  if (field("updateHours").is_number_integer()) {
    profile.updateHours = std::clamp(field("updateHours").get<int>(), 1, 24 * 7);
  }
  if (field("protocol").is_string() && settings.protocol.empty()) {
    settings.protocol = field("protocol").get<std::string>();
  }
  // Moved from before profiles: the one there was. Dropped in by hand: on
  // only if nothing else is - it doesn't join a running set by surprise.
  profile.enabled = legacy.is_object() || std::none_of(settings.profiles.begin(), settings.profiles.end(),
                                                       [](const Profile& p) { return p.enabled; });
  try {
    const std::filesystem::path dir = ProfileDir(profile.id);
    for (const wchar_t* name : {L"config.json", L"subscription.json", L"subscription.new.json"}) {
      const std::filesystem::path from = root / name;
      if (std::filesystem::is_regular_file(from, ec)) {
        THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(from.c_str(), (dir / name).c_str(),
                                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED));
      }
    }
    // The old history goes with the old configuration.
    if (legacy.is_object() && std::filesystem::is_directory(root / L"history", ec) &&
        !std::filesystem::exists(dir / L"history", ec)) {
      std::filesystem::rename(root / L"history", dir / L"history", ec);
    }
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("moving the configuration into its profile failed");
    return;  // tried again on the next start
  }
  settings.profiles.push_back(profile);
  try {
    SaveSettings(settings);  // tray.json knows the profile now: nothing is moved twice
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("saving tray.json after moving the configuration failed");
  }
}

}  // namespace

namespace {

// A secret for tray.json: encrypted for this Windows user (DPAPI), base64.
// Another user, or a copy of the file on another machine, can't read it.
std::string ProtectSecret(std::string_view secret) {
  DATA_BLOB in{.cbData = static_cast<DWORD>(secret.size()),
               .pbData = reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))};
  DATA_BLOB out{};
  if (secret.empty() || !CryptProtectData(&in, L"Sovereign", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return {};
  }
  const wil::unique_hlocal_ptr<BYTE> owned(out.pbData);
  return EncodeBase64(std::string_view(reinterpret_cast<const char*>(out.pbData), out.cbData));
}

std::string UnprotectSecret(std::string_view stored) {
  const auto blob = DecodeBase64(stored);
  if (!blob || blob->empty()) {
    return {};
  }
  DATA_BLOB in{.cbData = static_cast<DWORD>(blob->size()),
               .pbData = reinterpret_cast<BYTE*>(const_cast<char*>(blob->data()))};
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return {};
  }
  const wil::unique_hlocal_ptr<BYTE> owned(out.pbData);
  return {reinterpret_cast<const char*>(out.pbData), out.cbData};
}

}  // namespace

TraySettings LoadSettings() {
  TraySettings settings;
  const auto text = ReadText(DataDir() / L"tray.json");
  const auto json = text ? nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false) : nlohmann::json();
  if (!json.is_object()) {
    AdoptRootConfig(settings, nlohmann::json());
    return settings;
  }
  // Field by field: one wrong type doesn't cost the others.
  if (const auto v = json.find("wantOn"); v != json.end() && v->is_boolean()) {
    settings.wantOn = v->get<bool>();
  }
  // 0.4.3 had one configuration in use ("activeProfile") and the server
  // pick in it; now all that are on run together, and the pick is one.
  std::string legacyActive;
  if (const auto v = json.find("activeProfile"); v != json.end() && v->is_string()) {
    legacyActive = v->get<std::string>();
  }
  const auto profiles = json.find("profiles");
  if (profiles != json.end()) {
    settings.profiles = ProfilesFromJson(*profiles, legacyActive);
  }
  if (const auto v = json.find("protocol"); v != json.end() && v->is_string()) {
    settings.protocol = v->get<std::string>();
  } else if (profiles != json.end() && profiles->is_array()) {
    for (const auto& entry : *profiles) {
      if (entry.is_object() && entry.value("id", nlohmann::json()) == legacyActive && entry.contains("protocol") &&
          entry["protocol"].is_string()) {
        settings.protocol = entry["protocol"].get<std::string>();
      }
    }
  }
  if (const auto v = json.find("appsMode"); v != json.end() && v->is_string()) {
    settings.appsMode = ParseAppsMode(v->get<std::string>());
  }
  if (const auto v = json.find("apps"); v != json.end() && v->is_array()) {
    for (const auto& app : *v) {
      if (app.is_string() && !app.get<std::string>().empty()) {
        settings.apps.push_back(app.get<std::string>());
      }
    }
  }
  if (const auto v = json.find("appPaths"); v != json.end() && v->is_object()) {
    for (const auto& [name, path] : v->items()) {
      if (path.is_string() && !path.get<std::string>().empty()) {
        SetAppPath(settings, name, path.get<std::string>());
      }
    }
  }

  if (const auto v = json.find("hideExitIp"); v != json.end() && v->is_boolean()) {
    settings.hideExitIp = v->get<bool>();
  }
  if (const auto v = json.find("killSwitch"); v != json.end() && v->is_boolean()) {
    settings.killSwitch = v->get<bool>();
  }
  if (const auto v = json.find("lanClosed"); v != json.end() && v->is_boolean()) {
    settings.lanClosed = v->get<bool>();
  } else if (const auto old = json.find("killSwitchLan"); old != json.end() && old->is_boolean()) {
    // Before: the kill switch without the local network - the same wish.
    settings.lanClosed = settings.killSwitch && !old->get<bool>();
  }
  if (const auto v = json.find("lanAllowed"); v != json.end() && v->is_array()) {
    for (const auto& host : *v) {
      const std::string address = host.is_object() ? host.value("address", std::string()) : std::string();
      if (!address.empty() && settings.lanAllowed.size() < kMaxLanAllowed) {
        settings.lanAllowed.push_back({.address = address, .name = host.value("name", std::string())});
      }
    }
  }
  if (const auto v = json.find("logLevel"); v != json.end() && v->is_string() && IsLogLevel(v->get<std::string>())) {
    settings.logLevel = v->get<std::string>();
  }
  if (const auto v = json.find("relay"); v != json.end() && v->is_object()) {
    const std::string url = v->value("url", std::string());
    const std::string key = v->contains("key") && (*v)["key"].is_string() ? UnprotectSecret((*v)["key"].get<std::string>())
                                                                          : std::string();
    if (IsRelayUrl(url) && IsRelayKey(key)) {
      settings.relayUrl = url;
      settings.relayKey = key;
    }
  }
  AdoptRootConfig(settings, profiles == json.end() ? json : nlohmann::json());
  if (const auto v = json.find("routing"); v != json.end()) {
    settings.routing = RoutingFromJson(*v);
  } else {
    // Routing is the client's now: its defaults, and the rules the
    // configurations had (a subscription's "these sites directly").
    for (const Profile& profile : settings.profiles) {
      try {
        if (const auto config = LoadConfig(ProfileDir(profile.id))) {
          MergeRules(settings.routing.rules, ImportRules(*config));
        }
      } catch (...) {
        LOG_CAUGHT_EXCEPTION_MSG("reading a configuration's rules failed");
      }
    }
  }
  MoveAppsToRules(settings);
  return settings;
}

void MoveAppsToRules(TraySettings& settings) {
  if (settings.appsMode != AppsMode::Exclude) {
    return;  // only the listed through the proxy: a mode of its own, kept as it is
  }
  for (std::string& app : settings.apps) {
    MergeRules(settings.routing.rules,
               {RouteRule{.domains = {}, .keywords = {}, .ips = {}, .processes = {std::move(app)},
                          .action = RouteRule::Action::Direct}});
  }
  settings.apps.clear();
}

void SaveSettings(const TraySettings& settings) {
  nlohmann::json json;
  json["wantOn"] = settings.wantOn;
  json["profiles"] = ProfilesToJson(settings.profiles);
  json["protocol"] = settings.protocol;
  json["routing"] = RoutingToJson(settings.routing);
  json["appsMode"] = std::string(AppsModeName(settings.appsMode));
  json["apps"] = settings.apps;
  json["appPaths"] = nlohmann::json::object();
  for (const auto& [app, path] : settings.appPaths) {
    json["appPaths"][app] = path;
  }

  json["hideExitIp"] = settings.hideExitIp;
  json["logLevel"] = settings.logLevel;
  json["killSwitch"] = settings.killSwitch;
  json["lanClosed"] = settings.lanClosed;
  json["lanAllowed"] = nlohmann::json::array();
  for (const LanHost& host : settings.lanAllowed) {
    json["lanAllowed"].push_back({{"address", host.address}, {"name", host.name}});
  }
  if (!settings.relayUrl.empty() && !settings.relayKey.empty()) {
    json["relay"] = {{"url", settings.relayUrl}, {"key", ProtectSecret(settings.relayKey)}};
  }
  // Names came from subscription servers: not UTF-8 must not throw.
  WriteTextAtomically(DataDir() / L"tray.json", json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
}

std::filesystem::path RuleSetPath(const std::string& tag) {
  THROW_HR_IF(E_INVALIDARG, !IsProfileId(tag));  // the same safe-name rule: [a-z0-9-]
  const std::filesystem::path dir = DataDir() / L"rules";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  THROW_HR_IF(HRESULT_FROM_WIN32(ec.value()), ec.operator bool());
  return dir / std::filesystem::path(tag + ".srs");
}

void SaveRuleSet(const std::string& tag, const std::string& bytes) { WriteTextAtomically(RuleSetPath(tag), bytes); }

std::optional<std::chrono::hours> RuleSetAge(const std::string& tag) {
  std::error_code ec;
  const auto written = std::filesystem::last_write_time(RuleSetPath(tag), ec);
  if (ec) {
    return std::nullopt;
  }
  const auto age = std::filesystem::file_time_type::clock::now() - written;
  return std::chrono::duration_cast<std::chrono::hours>(age);
}

std::filesystem::path ProfileDir(const std::string& id) {
  THROW_HR_IF(E_INVALIDARG, !IsProfileId(id));
  const std::filesystem::path dir = DataDir() / L"profiles" / std::filesystem::path(id);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  THROW_HR_IF(HRESULT_FROM_WIN32(ec.value()), ec.operator bool());
  return dir;
}

void RemoveProfileDir(const std::string& id) {
  THROW_HR_IF(E_INVALIDARG, !IsProfileId(id));
  std::error_code ec;
  std::filesystem::remove_all(DataDir() / L"profiles" / std::filesystem::path(id), ec);
  THROW_HR_IF(HRESULT_FROM_WIN32(ec.value()), ec.operator bool());
}

std::optional<std::string> LoadConfig(const std::filesystem::path& dir) { return ReadText(dir / L"config.json"); }

void SaveConfig(const std::filesystem::path& dir, const std::string& text) {
  WriteTextAtomically(dir / L"config.json", text);
}

std::optional<std::string> LoadOriginal(const std::filesystem::path& dir) {
  return ReadText(dir / L"subscription.json");
}

void SaveOriginal(const std::filesystem::path& dir, const std::string& text) {
  WriteTextAtomically(dir / L"subscription.json", text);
}

std::optional<std::string> LoadPending(const std::filesystem::path& dir) {
  return ReadText(dir / L"subscription.new.json");
}

void SavePending(const std::filesystem::path& dir, const std::string& text) {
  WriteTextAtomically(dir / L"subscription.new.json", text);
}

void ClearPending(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::remove(dir / L"subscription.new.json", ec);  // no file: false, not an error
  THROW_HR_IF(HRESULT_FROM_WIN32(ec.value()), ec.operator bool());
}

void ClearOriginal(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::remove(dir / L"subscription.json", ec);  // no file: false, not an error
  THROW_HR_IF(HRESULT_FROM_WIN32(ec.value()), ec.operator bool());
}

void SaveHistory(const std::filesystem::path& base, const std::string& text) {
  const std::filesystem::path dir = base / L"history";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  THROW_HR_IF(HRESULT_FROM_WIN32(ec.value()), ec.operator bool());
  SYSTEMTIME now{};
  GetSystemTime(&now);
  WriteTextAtomically(dir / std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.json", now.wYear, now.wMonth, now.wDay,
                                         now.wHour, now.wMinute, now.wSecond, now.wMilliseconds),
                      text);

  // The names sort by time: drop the oldest. A file that won't go stays for
  // the next time.
  std::vector<std::filesystem::path> kept;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() == L".json") {
      kept.push_back(entry.path());
    }
  }
  std::sort(kept.begin(), kept.end());
  for (std::size_t i = 0; i + kHistoryKeep < kept.size(); ++i) {
    std::filesystem::remove(kept[i], ec);
  }
}
}  // namespace sovereign::tray
