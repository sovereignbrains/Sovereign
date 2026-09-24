#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "core.h"

namespace sovereign::service {

// ICore backed by sovereign-gocore.dll: sing-box hosted as a c-shared Go DLL
// (see gocore/). Talks to it only through its C exports.
class GoCore final : public ICore {
 public:
  // Loads the DLL from `dllPath` (an absolute path — see the comment on
  // ResolveGoCoreDllPath in go_core.cpp for why we don't rely on implicit
  // DLL search order). Throws wil::ResultException on failure.
  explicit GoCore(const std::wstring& dllPath);

  // Unregisters the log callback before the object goes away; the DLL
  // guarantees no callback into `this` is running or can start afterward.
  ~GoCore() override;

  GoCore(const GoCore&) = delete;
  GoCore& operator=(const GoCore&) = delete;
  GoCore(GoCore&&) = delete;
  GoCore& operator=(GoCore&&) = delete;

  // Strings returned by the DLL are allocated with its own (MinGW/UCRT)
  // allocator; each is copied into a std::string and freed via the DLL's own
  // box_free — never via the host's CRT free(), which would be undefined
  // behavior across the CRT boundary.
  std::string Ping() override;
  std::string Start(const std::string& configJson) override;
  std::string Stop() override;
  CoreStats Stats() override;
  void SetLogSink(LogSink sink) override;

 private:
  using BoxPingFn = char*(__cdecl*)();
  using BoxStartFn = char*(__cdecl*)(const char*);
  using BoxStopFn = char*(__cdecl*)();
  using BoxStatsFn = int(__cdecl*)(std::int64_t*, std::int64_t*, std::int64_t*, std::int64_t*);
  using LogCallbackFn = void(__cdecl*)(void*, int, const char*);
  using BoxSetLogCallbackFn = void(__cdecl*)(LogCallbackFn, void*);
  using BoxFreeFn = void(__cdecl*)(char*);

  // Entry point handed to box_set_log_callback; `context` is `this`. Runs on
  // sing-box's goroutines, so it must not throw across the C boundary.
  static void __cdecl OnLog(void* context, int level, const char* message) noexcept;

  // Copies a DLL-allocated string into a std::string and releases the
  // original via boxFree_ — the single chokepoint every string-returning
  // export routes through, so the cross-CRT-free rule can't be forgotten on
  // a new method.
  std::string TakeOwnedString(char* dllString);

  wil::unique_hmodule module_;
  BoxPingFn boxPing_ = nullptr;
  BoxStartFn boxStart_ = nullptr;
  BoxStopFn boxStop_ = nullptr;
  BoxStatsFn boxStats_ = nullptr;
  BoxSetLogCallbackFn boxSetLogCallback_ = nullptr;
  BoxFreeFn boxFree_ = nullptr;

  // Held while the sink runs and while it is swapped: once SetLogSink
  // returns, the previous sink is not running anywhere.
  std::mutex sinkMutex_;
  LogSink sink_;
};

// Computes the absolute path to sovereign-gocore.dll next to the running
// executable.
std::wstring ResolveGoCoreDllPath();

}  // namespace sovereign::service
