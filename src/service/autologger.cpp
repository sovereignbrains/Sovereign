#include "autologger.h"

#include <windows.h>

#include <evntrace.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <cstddef>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "trace.h"

namespace sovereign::service {

namespace {

// Documented in "Configuring and Starting an AutoLogger Session" (ETW docs).
constexpr wchar_t kRegistryPath[] = L"SYSTEM\\CurrentControlSet\\Control\\WMI\\Autologger\\Sovereign-Core";

// Identifies the session itself (AutoLogger requires one; any fixed GUID).
constexpr GUID kSessionId{0x879e2fa9, 0x8859, 0x4f74, {0xbb, 0x52, 0x6e, 0xbb, 0x3f, 0x2b, 0x22, 0x28}};

constexpr DWORD kMaxFileSizeMb = 16;
constexpr DWORD kFileMax = 4;       // sovereign.etl.0001..0004, one per boot
constexpr DWORD kFlushSeconds = 1;  // what a crash or power loss can cost at most
constexpr UCHAR kLevel = TRACE_LEVEL_INFORMATION;

std::wstring GuidString(const GUID& g) {
  return std::format(L"{{{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}}}",
                     g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                     g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

// %ProgramData%\Sovereign\Logs, created if missing (AutoLogger requires the
// directory to exist; it doesn't create it).
std::wstring EnsureLogDirectory() {
  wchar_t programData[MAX_PATH]{};
  const DWORD n = ExpandEnvironmentStringsW(L"%ProgramData%", programData, MAX_PATH);
  THROW_LAST_ERROR_IF(n == 0 || n > MAX_PATH);
  std::wstring dir = std::wstring(programData) + L"\\Sovereign";
  for (const std::wstring& d : {dir, dir + L"\\Logs"}) {
    if (!CreateDirectoryW(d.c_str(), nullptr)) {
      THROW_LAST_ERROR_IF(GetLastError() != ERROR_ALREADY_EXISTS);
    }
  }
  return dir + L"\\Logs";
}

void SetDword(HKEY key, const wchar_t* name, DWORD value) {
  THROW_IF_WIN32_ERROR(RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                                      sizeof value));
}

void SetQword(HKEY key, const wchar_t* name, ULONGLONG value) {
  THROW_IF_WIN32_ERROR(RegSetValueExW(key, name, 0, REG_QWORD, reinterpret_cast<const BYTE*>(&value),
                                      sizeof value));
}

void SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
  THROW_IF_WIN32_ERROR(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))));
}

// EVENT_TRACE_PROPERTIES with room for the session name and the file name
// after it, as StartTrace/ControlTrace expect.
class SessionProperties {
 public:
  explicit SessionProperties(const std::wstring& logFile = {}) : buffer_(kSize) {
    auto* p = Get();
    p->Wnode.BufferSize = static_cast<ULONG>(kSize);
    p->Wnode.Guid = kSessionId;
    p->Wnode.ClientContext = 1;  // QPC timestamps
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->LogFileMode = EVENT_TRACE_FILE_MODE_CIRCULAR;
    p->MaximumFileSize = kMaxFileSizeMb;
    p->FlushTimer = kFlushSeconds;
    p->LoggerNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES));
    p->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + kNameBytes);
    THROW_HR_IF(E_INVALIDARG, (logFile.size() + 1) * sizeof(wchar_t) > kNameBytes);
    std::memcpy(buffer_.data() + p->LogFileNameOffset, logFile.c_str(), (logFile.size() + 1) * sizeof(wchar_t));
  }

  EVENT_TRACE_PROPERTIES* Get() { return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer_.data()); }

 private:
  static constexpr std::size_t kNameBytes = 1024 * sizeof(wchar_t);
  static constexpr std::size_t kSize = sizeof(EVENT_TRACE_PROPERTIES) + 2 * kNameBytes;
  std::vector<std::byte> buffer_;
};

// The AutoLogger registry key takes effect at the next boot; this starts the
// same session now. Already running (a previous install, or this boot's
// AutoLogger) is fine: the provider is (re-)enabled on it either way.
void StartSessionNow(const std::wstring& logFile) {
  SessionProperties props(logFile);
  TRACEHANDLE session = 0;
  const ULONG started = StartTraceW(&session, kAutoLoggerName, props.Get());
  if (started == ERROR_ALREADY_EXISTS) {
    SessionProperties query;
    THROW_IF_WIN32_ERROR(ControlTraceW(0, kAutoLoggerName, query.Get(), EVENT_TRACE_CONTROL_QUERY));
    session = query.Get()->Wnode.HistoricalContext;
  } else {
    THROW_IF_WIN32_ERROR(started);
  }
  THROW_IF_WIN32_ERROR(EnableTraceEx2(session, &trace::kProviderId, EVENT_CONTROL_CODE_ENABLE_PROVIDER, kLevel,
                                      trace::kDefaultKeywords, 0, 0, nullptr));
}

}  // namespace

void InstallAutoLogger() {
  const std::wstring logFile = EnsureLogDirectory() + L"\\sovereign.etl";

  wil::unique_hkey session;
  THROW_IF_WIN32_ERROR(RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, nullptr, 0,
                                       KEY_SET_VALUE | KEY_CREATE_SUB_KEY, nullptr, &session, nullptr));
  SetString(session.get(), L"Guid", GuidString(kSessionId));
  SetString(session.get(), L"FileName", logFile);
  SetDword(session.get(), L"FileMax", kFileMax);
  SetDword(session.get(), L"MaxFileSize", kMaxFileSizeMb);
  SetDword(session.get(), L"LogFileMode", EVENT_TRACE_FILE_MODE_CIRCULAR);
  SetDword(session.get(), L"FlushTimer", kFlushSeconds);
  SetDword(session.get(), L"Start", 1);

  wil::unique_hkey provider;
  THROW_IF_WIN32_ERROR(RegCreateKeyExW(session.get(), GuidString(trace::kProviderId).c_str(), 0, nullptr, 0,
                                       KEY_SET_VALUE, nullptr, &provider, nullptr));
  SetDword(provider.get(), L"Enabled", 1);
  SetDword(provider.get(), L"EnableLevel", kLevel);
  SetQword(provider.get(), L"MatchAnyKeyword", trace::kDefaultKeywords);

  StartSessionNow(logFile);
}

void UninstallAutoLogger() {
  SessionProperties props;
  const ULONG stopped = ControlTraceW(0, kAutoLoggerName, props.Get(), EVENT_TRACE_CONTROL_STOP);
  if (stopped != ERROR_WMI_INSTANCE_NOT_FOUND) {
    THROW_IF_WIN32_ERROR(stopped);
  }
  const LONG deleted = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kRegistryPath);
  if (deleted != ERROR_FILE_NOT_FOUND) {
    THROW_IF_WIN32_ERROR(deleted);
  }
}

}  // namespace sovereign::service
