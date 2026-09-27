#include "autostart.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wil/resource.h>
#include <wil/result.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>

#include <array>
#include <string>

namespace sovereign::tray {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Where Task Manager's Startup tab keeps its on/off switch for Run entries: a
// binary value whose first byte is odd when the user turned the entry off.
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValueName[] = L"Sovereign";

bool DisabledInTaskManager() {
  std::array<BYTE, 12> state{};
  DWORD size = static_cast<DWORD>(state.size());
  return RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName, RRF_RT_REG_BINARY, nullptr, state.data(), &size) ==
             ERROR_SUCCESS &&
         size > 0 && (state[0] & 1U) != 0;
}

}  // namespace

bool AutostartEnabled() {
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
             ERROR_SUCCESS &&
         !DisabledInTaskManager();
}

bool SetAutostart(bool enabled) {
  // Either way Task Manager's own switch goes: turning on here must not stay
  // off there, and turning off leaves nothing behind.
  RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName);

  wil::unique_hkey key;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, key.put(), nullptr) !=
      ERROR_SUCCESS) {
    return false;
  }
  if (!enabled) {
    const LSTATUS status = RegDeleteValueW(key.get(), kValueName);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
  }
  std::wstring exe;
  try {
    exe = wil::GetModuleFileNameW<std::wstring>(nullptr);
  } catch (...) {
    LOG_CAUGHT_EXCEPTION();
    return false;
  }
  const std::wstring command = L"\"" + exe + L"\"";
  const auto bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
  return RegSetValueExW(key.get(), kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), bytes) ==
         ERROR_SUCCESS;
}

}  // namespace sovereign::tray
