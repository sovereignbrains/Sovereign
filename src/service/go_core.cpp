#include "go_core.h"

#include <wil/result.h>

#include <cstdint>
#include <utility>

namespace sovereign::service {

namespace {

// GetProcAddress always returns FARPROC; casting it to the export's real
// signature is the only way to call it and is inherent to the Win32 API
// shape. The intermediate uintptr_t cast (rather than a direct
// FARPROC->target reinterpret_cast) is deliberate: clang's
// cast-function-type-mismatch diagnostic is a hard error here (not a
// suppressible lint), and going through an integer breaks the pattern it
// matches on.
template <typename Fn>
Fn ResolveExport(HMODULE module, const char* name) {
  const FARPROC proc = GetProcAddress(module, name);
  THROW_LAST_ERROR_IF(!proc);
  return reinterpret_cast<Fn>(  // NOLINT(performance-no-int-to-ptr)
      reinterpret_cast<std::uintptr_t>(proc));
}

}  // namespace

std::wstring ResolveGoCoreDllPath() {
  wchar_t path[MAX_PATH]{};
  THROW_LAST_ERROR_IF(GetModuleFileNameW(nullptr, path, MAX_PATH) == 0);

  std::wstring exePath(path);
  const size_t lastSlash = exePath.find_last_of(L'\\');
  const std::wstring dir = (lastSlash == std::wstring::npos) ? L"." : exePath.substr(0, lastSlash);
  return dir + L"\\sovereign-gocore.dll";
}

GoCore::GoCore(const std::wstring& dllPath) {
  // LoadLibraryW with an absolute path we computed ourselves (never a
  // bare filename) — deliberately sidesteps Windows' default DLL search
  // order, which would otherwise probe the current working directory and
  // other locations an attacker could plant a DLL in.
  module_.reset(LoadLibraryW(dllPath.c_str()));
  THROW_LAST_ERROR_IF(!module_);

  boxPing_ = ResolveExport<BoxPingFn>(module_.get(), "box_ping");
  boxStart_ = ResolveExport<BoxStartFn>(module_.get(), "box_start");
  boxStop_ = ResolveExport<BoxStopFn>(module_.get(), "box_stop");
  boxStats_ = ResolveExport<BoxStatsFn>(module_.get(), "box_stats");
  boxSetLogCallback_ = ResolveExport<BoxSetLogCallbackFn>(module_.get(), "box_set_log_callback");
  boxFree_ = ResolveExport<BoxFreeFn>(module_.get(), "box_free");

  // Registered once for the object's lifetime; SetLogSink only swaps what
  // OnLog forwards to, so installing a sink never has to cross into Go.
  boxSetLogCallback_(&GoCore::OnLog, this);
}

GoCore::~GoCore() { boxSetLogCallback_(nullptr, nullptr); }

void __cdecl GoCore::OnLog(void* context, int level, const char* message) noexcept {
  auto* self = static_cast<GoCore*>(context);
  try {
    const std::scoped_lock lock(self->sinkMutex_);
    if (self->sink_) {
      self->sink_(static_cast<LogLevel>(level), message);
    }
  } catch (...) {
    // A sink failure must not unwind into Go's stack (undefined behavior
    // across the cgo boundary); a lost log line is the lesser evil. WIL
    // reports it (to ETW in the service, see trace.h).
    LOG_CAUGHT_EXCEPTION_MSG("log sink threw, line dropped");
  }
}

void GoCore::SetLogSink(LogSink sink) {
  const std::scoped_lock lock(sinkMutex_);
  sink_ = std::move(sink);
}

std::string GoCore::TakeOwnedString(char* dllString) {
  if (!dllString) {
    return {};
  }
  std::string result(dllString);
  boxFree_(dllString);
  return result;
}

std::string GoCore::Ping() { return TakeOwnedString(boxPing_()); }

std::string GoCore::Start(const std::string& configJson) {
  return TakeOwnedString(boxStart_(configJson.c_str()));
}

std::string GoCore::Stop() { return TakeOwnedString(boxStop_()); }

CoreStats GoCore::Stats() {
  CoreStats stats;
  stats.running = boxStats_(&stats.uplinkBytes, &stats.downlinkBytes, &stats.activeConnections,
                            &stats.generation) != 0;
  return stats;
}

}  // namespace sovereign::service
