#include "settings.h"

#include <windows.h>

#include <wil/result.h>

#include <nlohmann/json.hpp>

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
  if (json.is_object()) {
    const auto wantOn = json.find("wantOn");
    if (wantOn != json.end() && wantOn->is_boolean()) {
      settings.wantOn = wantOn->get<bool>();
    }
  }
  return settings;
}

void SaveSettings(const TraySettings& settings) {
  nlohmann::json json;
  json["wantOn"] = settings.wantOn;
  WriteTextAtomically(DataDir() / L"tray.json", json.dump(2) + "\n");
}

std::optional<std::string> LoadConfig() { return ReadText(DataDir() / L"config.json"); }

}  // namespace sovereign::tray
