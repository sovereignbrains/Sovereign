#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <string>

namespace sovereign::service {

// Loads sovereign-gocore.dll (sing-box hosted as a c-shared Go DLL, see
// gocore/) and calls its C exports. P2 atom 1a: only box_ping is wired up
// — proves the DLL loads inside the service process and a call round-trips
// without crashing. box_start/box_stop/box_stats land in 1b/1c.
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

 private:
  using BoxPingFn = char*(__cdecl*)();
  using BoxFreeFn = void(__cdecl*)(char*);

  wil::unique_hmodule module_;
  BoxPingFn boxPing_ = nullptr;
  BoxFreeFn boxFree_ = nullptr;
};

// Computes the absolute path to sovereign-gocore.dll next to the running
// executable.
std::wstring ResolveGoCoreDllPath();

}  // namespace sovereign::service
