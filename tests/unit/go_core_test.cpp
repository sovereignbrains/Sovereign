// GoCore's C++ side against gocore-stub.dll: loading by absolute path, export
// resolution, the TakeOwnedString/box_free chokepoint, stats marshalling and
// the log-callback lifetime. The real sovereign-gocore.dll can't be hosted in
// an ASan process (issue #9), and this is the part of the bridge ASan is for;
// the real DLL is exercised by the non-ASan smoke run (tests/smoke).

#include <windows.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "check.h"
#include "go_core.h"

namespace {

using sovereign::service::GoCore;
using sovereign::service::LogLevel;

std::wstring ExeDir() {
  wchar_t path[MAX_PATH]{};
  THROW_LAST_ERROR_IF(GetModuleFileNameW(nullptr, path, MAX_PATH) == 0);
  const std::wstring exe(path);
  return exe.substr(0, exe.find_last_of(L'\\'));
}

// Same integer round-trip as go_core.cpp's ResolveExport, for the same reason
// (clang's cast-function-type-mismatch is a hard error, not a lint).
template <typename Fn>
Fn StubExport(HMODULE module, const char* name) {
  const FARPROC proc = GetProcAddress(module, name);
  THROW_LAST_ERROR_IF(!proc);
  return reinterpret_cast<Fn>(  // NOLINT(performance-no-int-to-ptr)
      reinterpret_cast<std::uintptr_t>(proc));
}

template <typename F>
bool Throws(F&& f) {
  try {
    std::forward<F>(f)();
  } catch (const wil::ResultException&) {
    return true;
  }
  return false;
}

struct StubControls {
  int(__cdecl* outstanding)() = nullptr;
  int(__cdecl* hasLogCallback)() = nullptr;
  void(__cdecl* lastConfig)(char*, std::size_t) = nullptr;
  void(__cdecl* emitLog)(int, const char*) = nullptr;
  void(__cdecl* emitLogFromThread)(int, const char*) = nullptr;
};

StubControls ResolveControls(HMODULE stub) {
  StubControls c;
  c.outstanding = StubExport<decltype(c.outstanding)>(stub, "stub_outstanding");
  c.hasLogCallback = StubExport<decltype(c.hasLogCallback)>(stub, "stub_has_log_callback");
  c.lastConfig = StubExport<decltype(c.lastConfig)>(stub, "stub_last_config");
  c.emitLog = StubExport<decltype(c.emitLog)>(stub, "stub_emit_log");
  c.emitLogFromThread = StubExport<decltype(c.emitLogFromThread)>(stub, "stub_emit_log_from_thread");
  return c;
}

void TestLoadFailures() {
  // No such file, and a real DLL without the gocore exports: both must fail
  // loudly at construction rather than leave a half-resolved core behind.
  CHECK(Throws([] { GoCore core(ExeDir() + L"\\no-such-gocore.dll"); }));

  wchar_t system[MAX_PATH]{};
  THROW_LAST_ERROR_IF(GetSystemDirectoryW(system, MAX_PATH) == 0);
  CHECK(Throws([&] { GoCore core(std::wstring(system) + L"\\version.dll"); }));
}

void TestBridge(const std::wstring& stubPath) {
  // Our own reference keeps the stub loaded after GoCore releases its module,
  // so the controls stay callable for the after-destruction checks below.
  const wil::unique_hmodule keepLoaded(LoadLibraryW(stubPath.c_str()));
  THROW_LAST_ERROR_IF(!keepLoaded);
  const StubControls stub = ResolveControls(keepLoaded.get());

  std::mutex seenMutex;
  std::vector<std::pair<LogLevel, std::string>> seen;

  {
    auto core = std::make_unique<GoCore>(stubPath);
    CHECK(stub.hasLogCallback() == 1);  // registered once, in the constructor

    // Every string export goes back through box_free: nothing outstanding
    // after any call (a CRT free() instead would leave the count up - ASan
    // itself doesn't report it, see gocore_stub.cpp).
    CHECK(core->Ping() == "pong (stub)");
    CHECK(stub.outstanding() == 0);

    const std::string config = R"({"log":{"disabled":true},"outbounds":[{"type":"direct","tag":"direct"}]})";
    CHECK(core->Start(config).empty());
    char passed[256]{};
    stub.lastConfig(passed, sizeof passed);
    CHECK(std::string(passed) == config);
    CHECK(core->Start(config) == "box already started; call box_stop first");
    CHECK(stub.outstanding() == 0);

    const auto running = core->Stats();
    CHECK(running.running);
    CHECK(running.uplinkBytes == 1000);
    CHECK(running.downlinkBytes == 2000);
    CHECK(running.activeConnections == 3);
    CHECK(running.generation == 1);

    CHECK(core->Stop().empty());
    const auto stopped = core->Stats();
    CHECK(!stopped.running);
    CHECK(stopped.generation == 1);
    CHECK(stub.outstanding() == 0);

    // Log lines reach the sink with their level, also from a foreign thread.
    core->SetLogSink([&](LogLevel level, std::string_view message) {
      const std::scoped_lock lock(seenMutex);
      seen.emplace_back(level, std::string(message));
    });
    stub.emitLog(static_cast<int>(LogLevel::Info), "same thread");
    stub.emitLogFromThread(static_cast<int>(LogLevel::Error), "foreign thread");
    {
      const std::scoped_lock lock(seenMutex);
      CHECK(seen.size() == 2);
      CHECK(seen.size() == 2 && seen[0] == std::pair(LogLevel::Info, std::string("same thread")));
      CHECK(seen.size() == 2 && seen[1] == std::pair(LogLevel::Error, std::string("foreign thread")));
    }

    // A throwing sink must not unwind into the caller (Go's stack in production).
    core->SetLogSink([](LogLevel, std::string_view) { throw std::runtime_error("sink failed"); });
    stub.emitLog(static_cast<int>(LogLevel::Warn), "dropped");

    // An empty sink stops delivery without unregistering the callback.
    core->SetLogSink({});
    stub.emitLog(static_cast<int>(LogLevel::Info), "nobody listens");
    CHECK(stub.hasLogCallback() == 1);
  }

  // The destructor unregistered the callback: a line emitted now must not
  // reach the destroyed GoCore (that would be a use-after-free under ASan).
  CHECK(stub.hasLogCallback() == 0);
  stub.emitLog(static_cast<int>(LogLevel::Info), "after destruction");
  const std::scoped_lock lock(seenMutex);
  CHECK(seen.size() == 2);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catches below
  try {
    TestLoadFailures();
    TestBridge(ExeDir() + L"\\gocore-stub.dll");
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  } catch (...) {
    std::cerr << "unexpected non-standard exception\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
