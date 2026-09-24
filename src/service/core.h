#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace sovereign::service {

// Numbering matches sing-box's log.Level (log/level.go) on purpose: GoCore
// passes the value straight through, and a future NativeCore has to speak
// the same scale for the tray to treat both cores alike.
enum class LogLevel : std::uint8_t { Panic = 0, Fatal, Error, Warn, Info, Debug, Trace };

constexpr std::string_view LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Panic: return "panic";
    case LogLevel::Fatal: return "fatal";
    case LogLevel::Error: return "error";
    case LogLevel::Warn: return "warn";
    case LogLevel::Info: return "info";
    case LogLevel::Debug: return "debug";
    case LogLevel::Trace: return "trace";
  }
  return "unknown";
}

// One stats snapshot. Counters belong to one Start(): they begin at zero on
// every start, and `generation` increments on every successful Start() so a
// consumer computing rates from deltas can tell a reset from idle traffic.
struct CoreStats {
  bool running = false;
  std::int64_t uplinkBytes = 0;
  std::int64_t downlinkBytes = 0;
  std::int64_t activeConnections = 0;
  std::int64_t generation = 0;
};

// Called for every log line the core emits (already filtered by the config's
// log level). May be invoked from any thread, concurrently, for as long as
// it is installed — implementations of the sink must be thread-safe.
using LogSink = std::function<void(LogLevel level, std::string_view message)>;

// The proxy engine behind the service, swapped in-process (architecture
// decision in issue #8): GoCore hosts sing-box as a c-shared DLL today; the
// optional NativeCore (P4) implements the same interface. The service and
// its control pipe only ever talk to this, never to a concrete core.
class ICore {
 public:
  ICore() = default;
  virtual ~ICore() = default;

  // A core is a process-wide engine with registered callbacks pointing back
  // at it — copying or moving one is never meaningful.
  ICore(const ICore&) = delete;
  ICore& operator=(const ICore&) = delete;
  ICore(ICore&&) = delete;
  ICore& operator=(ICore&&) = delete;

  // A short liveness/identity string (e.g. which engine and version).
  virtual std::string Ping() = 0;

  // Starts the engine from a full sing-box config document. Returns an empty
  // string on success or an error message. Fails if already running.
  virtual std::string Start(const std::string& configJson) = 0;

  // Stops the engine. Empty string on success, including when not running.
  virtual std::string Stop() = 0;

  virtual CoreStats Stats() = 0;

  // Installs (or, with an empty sink, removes) the log sink. After this
  // returns, the previous sink is never called again.
  virtual void SetLogSink(LogSink sink) = 0;
};

}  // namespace sovereign::service
