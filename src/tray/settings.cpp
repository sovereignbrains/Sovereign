#include "settings.h"

#include <windows.h>

#include <wil/result.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

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

TraySettings LoadSettings() {
  TraySettings settings;
  const auto text = ReadText(DataDir() / L"tray.json");
  if (!text) {
    return settings;
  }
  const auto json = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    return settings;
  }
  // Field by field: one wrong type doesn't cost the others.
  if (const auto v = json.find("wantOn"); v != json.end() && v->is_boolean()) {
    settings.wantOn = v->get<bool>();
  }
  if (const auto v = json.find("subscriptionUrl"); v != json.end() && v->is_string()) {
    settings.subscriptionUrl = v->get<std::string>();
  }
  if (const auto v = json.find("lastRefresh"); v != json.end() && v->is_number_integer()) {
    settings.lastRefresh = v->get<std::int64_t>();
  }
  if (const auto v = json.find("updateHours"); v != json.end() && v->is_number_integer()) {
    settings.updateHours = std::clamp(v->get<int>(), 1, 24 * 7);
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
  if (const auto v = json.find("protocol"); v != json.end() && v->is_string()) {
    settings.protocol = v->get<std::string>();
  }
  return settings;
}

void SaveSettings(const TraySettings& settings) {
  nlohmann::json json;
  json["wantOn"] = settings.wantOn;
  json["subscriptionUrl"] = settings.subscriptionUrl;
  json["lastRefresh"] = settings.lastRefresh;
  json["updateHours"] = settings.updateHours;
  json["appsMode"] = std::string(AppsModeName(settings.appsMode));
  json["apps"] = settings.apps;
  json["protocol"] = settings.protocol;
  WriteTextAtomically(DataDir() / L"tray.json", json.dump(2) + "\n");
}

std::optional<std::string> LoadConfig() { return ReadText(DataDir() / L"config.json"); }

void SaveConfig(const std::string& text) { WriteTextAtomically(DataDir() / L"config.json", text); }

}  // namespace sovereign::tray
