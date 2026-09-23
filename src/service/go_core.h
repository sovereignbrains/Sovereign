#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <string>

namespace sovereign::service {

// Loads sovereign-gocore.dll (sing-box hosted as a c-shared Go DLL, see
// gocore/) and calls its C exports. P2 atom 1a wired up box_ping. P2 atom 1b
// adds box_start/box_stop, backed by a real sing-box instance. box_stats and
// TUN wiring are still 1c.
class GoCore {
 public:
  // Loads the DLL from `dllPath` (an absolute path — see the comment on
  // ResolveDllPath in go_core.cpp for why we don't rely on implicit DLL
  // search order). Throws wil::ResultException on failure.
  explicit GoCore(const std::wstring& dllPath);

  // Calls the DLL's box_ping export and returns its reply. The DLL
  // allocates the returned string with its own (MinGW/UCRT) allocator;
  // this copies it into a std::string and frees the original via the
  // DLL's own box_free export before returning — never via the host's
  // CRT free(), which would be undefined behavior across the CRT
  // boundary.
  std::string Ping();

  // Starts a sing-box instance from configJson (a full sing-box config
  // document — see gocore/main.go's box_start doc comment). Returns an
  // empty string on success or an error message on failure. Calling this
  // while an instance is already running returns an error rather than
  // replacing it — call Stop() first.
  std::string Start(const std::string& configJson);

  // Stops the running instance, if any. Returns an empty string on success
  // (including when nothing was running) or an error message on failure.
  std::string Stop();

 private:
  using BoxPingFn = char*(__cdecl*)();
  using BoxStartFn = char*(__cdecl*)(const char*);
  using BoxStopFn = char*(__cdecl*)();
  using BoxFreeFn = void(__cdecl*)(char*);

  // Copies a DLL-allocated string into a std::string and releases the
  // original via boxFree_ — the single chokepoint every export in this
  // class routes its return value through, so the cross-CRT-free rule
  // (see the class doc comment) can't be forgotten on a new method.
  std::string TakeOwnedString(char* dllString);

  wil::unique_hmodule module_;
  BoxPingFn boxPing_ = nullptr;
  BoxStartFn boxStart_ = nullptr;
  BoxStopFn boxStop_ = nullptr;
  BoxFreeFn boxFree_ = nullptr;
};

// Computes the absolute path to sovereign-gocore.dll next to the running
// executable.
std::wstring ResolveGoCoreDllPath();

}  // namespace sovereign::service
