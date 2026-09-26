#include "trace.h"

#include <TraceLoggingProvider.h>
#include <winmeta.h>

#include <wil/result.h>

#include <algorithm>
#include <cstddef>

// {356e995a-3c2d-5ae3-fad1-410ee24d609d} = the name hash of "Sovereign.Core".
TRACELOGGING_DEFINE_PROVIDER(g_sovereignProvider, "Sovereign.Core",
                             (0x356e995a, 0x3c2d, 0x5ae3, 0xfa, 0xd1, 0x41, 0x0e, 0xe2, 0x4d, 0x60,
                              0x9d));

namespace sovereign::service::trace {

namespace {

// A string_view in the shape TraceLogging's counted-string fields take: the
// pointer and a 16-bit length (longer text is cut, not dropped). The text
// doesn't have to be terminated - the length always travels with it.
struct Counted {
  const char* data;
  USHORT size;

  static Counted Of(std::string_view text) {
    return {text.data(),  // NOLINT(bugprone-suspicious-stringview-data-usage) - size is passed along
            static_cast<USHORT>(std::min<std::size_t>(text.size(), 0xFFFF))};
  }
};

// Every failure WIL sees (THROW_*, RETURN_*, LOG_*, fail-fast) - including the
// ones that end up caught and handled, which is exactly the context a log
// needs. Runs on the failing thread; TraceLoggingWrite doesn't throw.
void __stdcall OnWilFailure(const wil::FailureInfo& failure) noexcept {
  TraceLoggingWrite(g_sovereignProvider, "Failure", TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                    TraceLoggingKeyword(kFailures),
                    TraceLoggingHResult(failure.hr, "HResult"),
                    TraceLoggingUInt32(static_cast<UINT32>(failure.type), "Type"),
                    TraceLoggingWideString(failure.pszMessage ? failure.pszMessage : L"", "Message"),
                    TraceLoggingString(failure.pszFile ? failure.pszFile : "", "File"),
                    TraceLoggingUInt32(failure.uLineNumber, "Line"),
                    TraceLoggingString(failure.pszFunction ? failure.pszFunction : "", "Function"));
}

}  // namespace

ProviderRegistration::ProviderRegistration() {
  // A failed registration leaves the provider handle inert (writes become
  // no-ops) - the service must run without logging rather than not at all.
  (void)TraceLoggingRegister(g_sovereignProvider);
  wil::SetResultLoggingCallback(&OnWilFailure);
}

ProviderRegistration::~ProviderRegistration() {
  wil::SetResultLoggingCallback(nullptr);
  TraceLoggingUnregister(g_sovereignProvider);
}

void ProcessState(std::string_view mode, std::string_view state) noexcept {
  TraceLoggingWrite(g_sovereignProvider, "ProcessState", TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                    TraceLoggingKeyword(kLifecycle),
                    TraceLoggingCountedUtf8String(Counted::Of(mode).data, Counted::Of(mode).size, "Mode"),
                    TraceLoggingCountedUtf8String(Counted::Of(state).data, Counted::Of(state).size, "State"));
}

void CoreLoaded() noexcept {
  TraceLoggingWrite(g_sovereignProvider, "CoreLoaded", TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                    TraceLoggingKeyword(kLifecycle));
}

void CoreLoadFailed(HRESULT hr, std::string_view message) noexcept {
  TraceLoggingWrite(g_sovereignProvider, "CoreLoadFailed", TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                    TraceLoggingKeyword(kLifecycle), TraceLoggingHResult(hr, "HResult"),
                    TraceLoggingCountedUtf8String(Counted::Of(message).data, Counted::Of(message).size, "Message"));
}

void Command(const CommandRecord& record) noexcept {
  // The level is compile-time metadata in TraceLogging, hence two writes.
  if (record.error.empty()) {
    TraceLoggingWrite(g_sovereignProvider, "Command", TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                      TraceLoggingKeyword(kControl),
                      TraceLoggingCountedUtf8String(Counted::Of(record.cmd).data, Counted::Of(record.cmd).size, "Cmd"),
                      TraceLoggingUInt64(record.requestBytes, "RequestBytes"),
                      TraceLoggingInt64(record.duration.count(), "DurationUs"));
  } else {
    TraceLoggingWrite(g_sovereignProvider, "CommandFailed", TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
                      TraceLoggingKeyword(kControl),
                      TraceLoggingCountedUtf8String(Counted::Of(record.cmd).data, Counted::Of(record.cmd).size, "Cmd"),
                      TraceLoggingUInt64(record.requestBytes, "RequestBytes"),
                      TraceLoggingInt64(record.duration.count(), "DurationUs"),
                      TraceLoggingCountedUtf8String(Counted::Of(record.error).data, Counted::Of(record.error).size, "Error"));
  }
}

void Power(std::string_view action, std::string_view error) noexcept {
  if (error.empty()) {
    TraceLoggingWrite(g_sovereignProvider, "Power", TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                      TraceLoggingKeyword(kPower),
                      TraceLoggingCountedUtf8String(Counted::Of(action).data, Counted::Of(action).size, "Action"));
  } else {
    TraceLoggingWrite(g_sovereignProvider, "PowerFailed", TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                      TraceLoggingKeyword(kPower),
                      TraceLoggingCountedUtf8String(Counted::Of(action).data, Counted::Of(action).size, "Action"),
                      TraceLoggingCountedUtf8String(Counted::Of(error).data, Counted::Of(error).size, "Error"));
  }
}

void CoreLog(LogLevel level, std::string_view message) noexcept {
  // Problems (warn and worse) and verbose lines go under different keywords:
  // info lines name every domain the user connects to, so a session has to
  // ask for them explicitly (kCoreVerbose is not in kDefaultKeywords).
  const std::string_view name = LogLevelName(level);
  switch (level) {
    case LogLevel::Panic:
    case LogLevel::Fatal:
      TraceLoggingWrite(g_sovereignProvider, "CoreLog", TraceLoggingLevel(WINEVENT_LEVEL_CRITICAL),
                        TraceLoggingKeyword(kCoreProblems),
                        TraceLoggingCountedUtf8String(Counted::Of(name).data, Counted::Of(name).size, "Level"),
                        TraceLoggingCountedUtf8String(Counted::Of(message).data, Counted::Of(message).size, "Message"));
      break;
    case LogLevel::Error:
      TraceLoggingWrite(g_sovereignProvider, "CoreLog", TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                        TraceLoggingKeyword(kCoreProblems),
                        TraceLoggingCountedUtf8String(Counted::Of(name).data, Counted::Of(name).size, "Level"),
                        TraceLoggingCountedUtf8String(Counted::Of(message).data, Counted::Of(message).size, "Message"));
      break;
    case LogLevel::Warn:
      TraceLoggingWrite(g_sovereignProvider, "CoreLog", TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
                        TraceLoggingKeyword(kCoreProblems),
                        TraceLoggingCountedUtf8String(Counted::Of(name).data, Counted::Of(name).size, "Level"),
                        TraceLoggingCountedUtf8String(Counted::Of(message).data, Counted::Of(message).size, "Message"));
      break;
    case LogLevel::Info:
      TraceLoggingWrite(g_sovereignProvider, "CoreLog", TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                        TraceLoggingKeyword(kCoreVerbose),
                        TraceLoggingCountedUtf8String(Counted::Of(name).data, Counted::Of(name).size, "Level"),
                        TraceLoggingCountedUtf8String(Counted::Of(message).data, Counted::Of(message).size, "Message"));
      break;
    case LogLevel::Debug:
    case LogLevel::Trace:
      TraceLoggingWrite(g_sovereignProvider, "CoreLog", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                        TraceLoggingKeyword(kCoreVerbose),
                        TraceLoggingCountedUtf8String(Counted::Of(name).data, Counted::Of(name).size, "Level"),
                        TraceLoggingCountedUtf8String(Counted::Of(message).data, Counted::Of(message).size, "Message"));
      break;
  }
}

void ShutdownStopFailed(std::string_view error) noexcept {
  TraceLoggingWrite(g_sovereignProvider, "ShutdownStopFailed", TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                    TraceLoggingKeyword(kLifecycle),
                    TraceLoggingCountedUtf8String(Counted::Of(error).data, Counted::Of(error).size, "Error"));
}

}  // namespace sovereign::service::trace
