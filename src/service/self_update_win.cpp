#include "self_update_win.h"

#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

#include "fetch.h"
#include "sha256.h"
#include "update.h"

namespace sovereign::service {

namespace {

constexpr std::size_t kMaxChecksumBytes = std::size_t{1024} * 1024;
constexpr std::uint64_t kMaxInstallerBytes = 512ULL * 1024 * 1024;

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n);
  return wide;
}

// The whole file, if it is a plain file of at most `limit` bytes.
std::optional<std::string> ReadFileAll(const std::wstring& path, std::uint64_t limit) {
  const wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  LARGE_INTEGER size{};
  if (!file || GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > limit) {
    return std::nullopt;
  }
  std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t offset = 0;
  while (offset < data.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1U << 20));
    DWORD read = 0;
    if (!ReadFile(file.get(), data.data() + offset, chunk, &read, nullptr) || read == 0) {
      return std::nullopt;
    }
    offset += read;
  }
  return data;
}

bool WriteFileAll(const std::wstring& path, const std::string& data) {
  const wil::unique_hfile file(
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1U << 20));
    DWORD written = 0;
    if (!WriteFile(file.get(), data.data() + offset, chunk, &written, nullptr) || written == 0) {
      return false;
    }
    offset += written;
  }
  return true;
}

}  // namespace

SelfUpdate::SelfUpdate(tray::Version current, std::wstring directory, std::wstring userAgent)
    : current_(current), directory_(std::move(directory)), userAgent_(std::move(userAgent)) {
  // The installers of the updates before: the last one ran from here and was
  // still running when this service was started again, so it goes now.
  if (directory_.empty()) {
    return;
  }
  WIN32_FIND_DATAW found{};
  const wil::unique_hfind find(FindFirstFileW((directory_ + L"\\Sovereign-Setup-*.exe").c_str(), &found));
  if (find) {
    do {
      DeleteFileW((directory_ + L"\\" + found.cFileName).c_str());
    } while (FindNextFileW(find.get(), &found));
  }
}

std::string SelfUpdate::Install(const std::string& version, const std::string& installer, std::uint32_t session) {
  const auto wanted = tray::ParseVersion(version);
  if (!wanted) {
    return "непонятная версия";
  }
  if (tray::IsDevelopmentBuild(current_)) {
    return "сборка для разработки не обновляется";
  }
  if (*wanted <= current_) {
    return "эта версия не новее установленной";
  }
  // What the installer must be: the release's own word, from GitHub.
  const auto checksumText = tray::Download(Widen(tray::ChecksumUrl(*wanted)), userAgent_, kMaxChecksumBytes);
  if (!checksumText) {
    return "контрольная сумма не скачалась: " + checksumText.error();
  }
  const std::string name = tray::InstallerName(*wanted);
  const auto expected = tray::ParseChecksum(*checksumText, name);
  if (!expected) {
    return "в файле контрольной суммы нет установщика";
  }
  const std::wstring source = Widen(installer);
  const auto bytes = source.empty() ? std::nullopt : ReadFileAll(source, kMaxInstallerBytes);
  if (!bytes) {
    return "служба не прочитала скачанный установщик";
  }
  bool matches = false;
  try {
    matches = Sha256Hex(*bytes) == *expected;
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("hashing the installer failed");
  }
  if (!matches) {
    return "установщик не совпал с контрольной суммой — не запускаю";
  }
  // Run from the program's own folder, not the user's: only administrators
  // may write there, so what was checked is what runs.
  if (directory_.empty() ||
      (!CreateDirectoryW(directory_.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)) {
    return "служба не создала папку для установщика";
  }
  const std::wstring path = directory_ + L"\\" + Widen(name);
  if (!WriteFileAll(path, *bytes)) {
    return "служба не сохранила установщик";
  }
  // /SESSION: the installer brings the tray back there (sovereign-core.exe
  // --launch-tray), not as SYSTEM in this service's session.
  std::wstring command =
      std::format(L"\"{}\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SESSION={}", path, session);
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  wil::unique_process_information process;
  if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      directory_.c_str(), &startup, &process)) {
    return "установщик не запустился";
  }
  return {};
}

namespace {

std::wstring ExeDirectory() {
  wchar_t path[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n == 0 || n == MAX_PATH) {
    return {};
  }
  std::wstring dir(path, n);
  return dir.substr(0, dir.find_last_of(L'\\'));
}

}  // namespace

std::wstring UpdateDirectory() {
  const std::wstring dir = ExeDirectory();
  return dir.empty() ? std::wstring() : dir + L"\\Update";
}

bool LaunchTray(std::uint32_t session) {
  // The user's own token (not elevated, for an administrator too): the tray
  // runs as when they start it.
  wil::unique_handle token;
  if (!WTSQueryUserToken(session, &token)) {
    return false;  // nobody signed in there any more
  }
  wil::unique_environment_block environment;
  if (!CreateEnvironmentBlock(&environment, token.get(), FALSE)) {
    return false;
  }
  const std::wstring dir = ExeDirectory();
  const std::wstring exe = dir + L"\\sovereign-tray.exe";
  std::wstring command = L"\"" + exe + L"\" --updated";
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
  wil::unique_process_information process;
  return CreateProcessAsUserW(token.get(), exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                              CREATE_UNICODE_ENVIRONMENT, environment.get(), dir.c_str(), &startup, &process) != FALSE;
}

}  // namespace sovereign::service
