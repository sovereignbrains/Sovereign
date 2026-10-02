#include "updater.h"

#include <shellapi.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include "fetch.h"
#include "sha256.h"

namespace sovereign::tray {

namespace {

constexpr std::chrono::minutes kFirstCheck{1};
constexpr std::chrono::hours kCheckEvery{12};
constexpr std::size_t kMaxInstallerBytes = 512ULL * 1024 * 1024;
constexpr std::size_t kMaxSmallBytes = std::size_t{1024} * 1024;

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n);
  return wide;
}

// %TEMP%\Sovereign-Update\<name>, the directory created; empty on failure.
std::wstring TempPath(const std::string& name) {
  wchar_t temp[MAX_PATH + 1]{};
  const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
  if (length == 0 || length > MAX_PATH) {
    return {};
  }
  std::wstring dir = std::wstring(temp, length) + L"Sovereign-Update";
  if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
    return {};
  }
  return dir + L"\\" + Widen(name);
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

Updater::Updater(Version current, std::wstring userAgent, std::function<void()> changed)
    : current_(current),
      userAgent_(std::move(userAgent)),
      changed_(std::move(changed)),
      thread_([this](const std::stop_token& stop) { Run(stop); }) {}

Updater::~Updater() {
  thread_.request_stop();
  wake_.notify_all();
}

void Updater::Check() {
  {
    const std::scoped_lock lock(mutex_);
    checkRequested_ = true;
  }
  wake_.notify_all();
}

void Updater::Install() {
  {
    const std::scoped_lock lock(mutex_);
    installRequested_ = true;
  }
  wake_.notify_all();
}

void Updater::LaunchFailed(const std::string& why) {
  Set([&](State& s) {
    s.status = Status::Available;
    s.error = why;
    s.installer.clear();
  });
}

Updater::State Updater::Get() const {
  const std::scoped_lock lock(mutex_);
  return state_;
}

void Updater::Set(const std::function<void(State&)>& change) {
  {
    const std::scoped_lock lock(mutex_);
    change(state_);
  }
  changed_();
}

void Updater::Run(const std::stop_token& stop) {
  auto next = std::chrono::steady_clock::now() + kFirstCheck;
  while (!stop.stop_requested()) {
    bool check = false;
    bool install = false;
    {
      std::unique_lock lock(mutex_);
      wake_.wait_until(lock, stop, next, [&] { return checkRequested_ || installRequested_; });
      if (stop.stop_requested()) {
        return;
      }
      check = std::exchange(checkRequested_, false) || std::chrono::steady_clock::now() >= next;
      install = std::exchange(installRequested_, false);
    }
    if (check) {
      next = std::chrono::steady_clock::now() + kCheckEvery;
      DoCheck();
    }
    if (install) {
      DoInstall();
    }
  }
}

void Updater::DoCheck() {
  Set([](State& s) {
    if (s.status != Status::Ready && s.status != Status::Downloading) {
      s.status = Status::Checking;
    }
  });
  const auto body = Download(Widen(std::string(kReleasesApi)), userAgent_, kMaxSmallBytes);
  const auto release = body ? ParseLatestRelease(*body) : std::nullopt;
  if (!release) {
    Set([&](State& s) {
      if (s.status == Status::Checking) {
        s.status = Status::Failed;
        s.error = body ? std::string("в последнем релизе нет установщика") : body.error();
      }
    });
    return;
  }
  const bool newer = release->version > current_;
  {
    const std::scoped_lock lock(mutex_);
    release_ = newer ? release : std::nullopt;
  }
  Set([&](State& s) {
    s.latest = ToString(release->version);
    if (s.status == Status::Checking) {
      s.status = newer ? Status::Available : Status::UpToDate;
      s.error.clear();
    }
  });
}

void Updater::DoInstall() {
  std::optional<Release> release;
  {
    const std::scoped_lock lock(mutex_);
    release = release_;
  }
  if (!release) {
    return;
  }
  Set([](State& s) {
    s.status = Status::Downloading;
    s.error.clear();
  });
  const auto fail = [this](const std::string& why) {
    Set([&](State& s) {
      s.status = Status::Available;
      s.error = why;
    });
  };
  const std::string name = InstallerName(release->version);
  const auto checksumText = Download(Widen(release->checksumUrl), userAgent_, kMaxSmallBytes);
  if (!checksumText) {
    return fail("контрольная сумма не скачалась: " + checksumText.error());
  }
  const auto expected = ParseChecksum(*checksumText, name);
  if (!expected) {
    return fail("в файле контрольной суммы нет установщика");
  }
  const auto installer = Download(Widen(release->installerUrl), userAgent_, kMaxInstallerBytes);
  if (!installer) {
    return fail("установщик не скачался: " + installer.error());
  }
  bool matches = release->installerSize == 0 || installer->size() == release->installerSize;
  try {
    matches = matches && Sha256Hex(*installer) == *expected;
  } catch (...) {
    LOG_CAUGHT_EXCEPTION_MSG("hashing the installer failed");
    matches = false;
  }
  if (!matches) {
    return fail("установщик не совпал с контрольной суммой — не запускаю");
  }
  const std::wstring path = TempPath(name);
  if (path.empty() || !WriteFileAll(path, *installer)) {
    return fail("не удалось сохранить установщик во временную папку");
  }
  Set([&](State& s) {
    s.status = Status::Ready;
    s.installer = path;
  });
}

std::string LaunchInstaller(HWND owner, const std::wstring& path) {
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof info;
  info.fMask = SEE_MASK_NOASYNC;
  info.hwnd = owner;
  info.lpVerb = L"runas";
  info.lpFile = path.c_str();
  info.lpParameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
  info.nShow = SW_SHOWNORMAL;
  if (ShellExecuteExW(&info)) {
    return {};
  }
  return GetLastError() == ERROR_CANCELLED ? std::string("установка отменена") : std::string("установщик не запустился");
}

}  // namespace sovereign::tray
