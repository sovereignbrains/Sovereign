#pragma once

// ETW for the service: one TraceLogging provider, "Sovereign.Core"
// {356e995a-3c2d-5ae3-fad1-410ee24d609d}. The GUID is the standard hash of the
// name (what EventSource and `*Name` in tracelog/WPR compute), so the provider
// can be enabled by name as well as by GUID. Self-describing events: no
// manifest to register, decoded by tracerpt/WPA/TDH as is.
//
// Nothing here writes a file. A session decides where events go - the
// AutoLogger registered by `sovereign-core --install`, or any ad-hoc session
// (tools/trace/sovtrace.ps1). With no session listening, every call is a
// cheap check of an enabled flag.
//
// Never pass a sing-box config or anything taken verbatim from a control
// request: configs carry passwords and keys (see CommandRecord in control.h).

#include <windows.h>

#include <cstdint>
#include <string_view>

#include "control.h"
#include "core.h"

namespace sovereign::service::trace {

// Keywords (bit mask a session enables). Values are part of the tooling
// contract: sovtrace.ps1 and the AutoLogger registry key use them.
inline constexpr std::uint64_t kLifecycle = 0x1;     // service/console start and stop, core load
inline constexpr std::uint64_t kControl = 0x2;       // one event per control-pipe request
inline constexpr std::uint64_t kPower = 0x4;         // suspend/resume and the box restart after it
inline constexpr std::uint64_t kCoreProblems = 0x8;  // sing-box's own warn/error/fatal/panic lines
inline constexpr std::uint64_t kCoreVerbose = 0x10;  // sing-box's info/debug/trace lines (domains!)
inline constexpr std::uint64_t kFailures = 0x20;     // every failure WIL reports (THROW_*, LOG_CAUGHT_*...)
// What the AutoLogger records by default: everything except kCoreVerbose,
// which names every domain the user visits.
inline constexpr std::uint64_t kDefaultKeywords =
    kLifecycle | kControl | kPower | kCoreProblems | kFailures;

// Registers the provider for this object's lifetime (one per process, in
// wmain) and routes WIL's failure reports to ETW while registered.
class ProviderRegistration {
 public:
  ProviderRegistration();
  ~ProviderRegistration();
  ProviderRegistration(const ProviderRegistration&) = delete;
  ProviderRegistration& operator=(const ProviderRegistration&) = delete;
  ProviderRegistration(ProviderRegistration&&) = delete;
  ProviderRegistration& operator=(ProviderRegistration&&) = delete;
};

// "service" (under SCM) or "console" (--run); state: "starting", "running",
// "stopping", "stopped".
void ProcessState(std::string_view mode, std::string_view state) noexcept;

void CoreLoaded() noexcept;
void CoreLoadFailed(HRESULT hr, std::string_view message) noexcept;

void Command(const CommandRecord& record) noexcept;

// action: "suspend", "resume", "restart_stop", "restart_start", "restart_skipped";
// error: empty on success.
void Power(std::string_view action, std::string_view error = {}) noexcept;

void CoreLog(LogLevel level, std::string_view message) noexcept;

// The box couldn't be stopped when the service shut down (TUN and routes may
// be left behind).
void ShutdownStopFailed(std::string_view error) noexcept;

}  // namespace sovereign::service::trace
