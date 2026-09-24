#include <windows.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include "control.h"
#include "core.h"
#include "go_core.h"
#include "log_ring.h"
#include "pipe_server.h"
#include "protocol.h"

namespace {

constexpr wchar_t kServiceName[] = L"SovereignCore";
constexpr wchar_t kServiceDisplayName[] = L"Sovereign Core";

// How many of the core's recent log lines the service keeps for the tray.
constexpr std::size_t kCoreLogCapacity = 2000;

// Живёт на весь процесс службы: ServiceCtrlHandler (обычный C-callback без
// контекста) должен куда-то дотянуться, поэтому состояние остановки хранится
// в объекте с статической длительностью, а не как локальная переменная.
class ServiceState {
 public:
  static ServiceState& Instance() {
    static ServiceState instance;
    return instance;
  }

  std::stop_source stopSource;
  SERVICE_STATUS_HANDLE statusHandle = nullptr;

  // Declared before `core` so it is destroyed after it: the core's log sink
  // points into this ring, and ~GoCore is what guarantees no further calls.
  sovereign::service::LogRing coreLog{kCoreLogCapacity};

  // The engine behind the service (issue #8: an in-process ICore — GoCore
  // today, NativeCore later). Null if it failed to load.
  std::unique_ptr<sovereign::service::ICore> core;

  // Set on the last successful box_start; PBT_APMRESUMEAUTOMATIC replays it.
  // Cleared on an explicit box_stop, so a resume after the user turned the
  // box off on purpose does not silently turn it back on.
  std::optional<std::string> lastConfig;
};

// The core is optional at this stage; if the DLL is missing or fails to
// load, the service still starts and the pipe still answers everything
// except the core commands — a missing dev-time artifact shouldn't take
// down the whole service.
void TryLoadCore(ServiceState& state) {
  try {
    state.core = std::make_unique<sovereign::service::GoCore>(
        sovereign::service::ResolveGoCoreDllPath());
    state.core->SetLogSink(
        [&log = state.coreLog](sovereign::service::LogLevel level, std::string_view message) {
          log.Append(level, message);
        });
  } catch (const wil::ResultException& e) {
    std::wcerr << L"GoCore недоступен: " << e.what() << L"\n";
  }
}

void ReportStatus(SERVICE_STATUS_HANDLE handle, DWORD state,
                   DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
  SERVICE_STATUS status{};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwControlsAccepted = (state == SERVICE_START_PENDING)
                                   ? 0
                                   : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_POWEREVENT;
  status.dwWin32ExitCode = exitCode;
  status.dwWaitHint = waitHint;
  SetServiceStatus(handle, &status);
}

// OutputDebugStringW needs a live listener (DebugView) attached at the exact
// moment the message is emitted — useless for proving what happened while
// the machine was asleep with nobody watching. A plain file survives that;
// %ProgramData% is writable by the SYSTEM service and, unlike a restricted
// install directory, still readable by a non-elevated session afterward.
void LogPowerEvent(const std::string& message) {
  const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  const std::string line = std::format("[{:%Y-%m-%d %H:%M:%S}] {}\n", now, message);
  OutputDebugStringA(line.c_str());
  try {
    std::filesystem::create_directories(L"C:\\ProgramData\\Sovereign");
    std::ofstream log("C:\\ProgramData\\Sovereign\\power-events.log", std::ios::app);
    log << line;
  } catch (...) {
    // Best-effort: a logging failure must not take the power-event path down,
    // but still surface it somewhere a live DebugView session would catch.
    OutputDebugStringA("sovereign-core: power-events.log write failed\n");
  }
}

DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD eventType, LPVOID, LPVOID) {
  auto& state = ServiceState::Instance();
  switch (control) {
    case SERVICE_CONTROL_STOP:
      ReportStatus(state.statusHandle, SERVICE_STOP_PENDING, NO_ERROR, 3000);
      state.stopSource.request_stop();
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT:
      switch (eventType) {
        case PBT_APMSUSPEND:
          LogPowerEvent("power: PBT_APMSUSPEND (going to sleep)");
          break;
        case PBT_APMRESUMEAUTOMATIC:
          LogPowerEvent("power: PBT_APMRESUMEAUTOMATIC (resuming)");
          // Always restart rather than probe whether the box survived sleep —
          // wintun's adapter and the route table are not guaranteed to survive
          // a suspend/resume cycle, and checking liveness first only adds a
          // second failure mode. Known gap: this runs on the SCM control
          // thread while a pipe box_start/box_stop (main thread) could be
          // in flight at the same instant — GoCore's Go-side mutex keeps that
          // memory-safe, but the two requests could still interleave in a
          // confusing order. Accepted for 1d-2: a client racing a sleep/wake
          // in that exact window is not a realistic scenario to design around
          // here.
          if (state.core && state.lastConfig) {
            const std::string stopError = state.core->Stop();
            LogPowerEvent(stopError.empty() ? "power: resume box_stop OK"
                                             : "power: resume box_stop error: " + stopError);
            const std::string startError = state.core->Start(*state.lastConfig);
            LogPowerEvent(startError.empty() ? "power: resume box_start OK"
                                              : "power: resume box_start error: " + startError);
          } else {
            LogPowerEvent("power: resume — nothing to restart (no core or no remembered config)");
          }
          break;
        default:
          break;
      }
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

void RunPipeServer(const std::stop_token& stopToken, ServiceState& state) {
  sovereign::service::ControlHandler handler(state.core.get(), state.coreLog, state.lastConfig);
  sovereign::service::PipeServer server(
      sovereign::ipc::kPipeName,
      [&handler](const std::string& request) { return handler.Handle(request); });
  server.Run(stopToken);
}

// Leaving a box running past the service's own shutdown would leave its TUN
// adapter and auto_route routes behind for the rest of the session — the
// machine's default route pointing at a dead interface. Stop it explicitly.
void StopCoreOnShutdown(ServiceState& state) {
  if (!state.core) {
    return;
  }
  const std::string error = state.core->Stop();
  if (!error.empty()) {
    OutputDebugStringA(("sovereign-core: box_stop on shutdown failed: " + error + "\n").c_str());
  }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
  auto& state = ServiceState::Instance();
  state.statusHandle =
      RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandler, nullptr);
  if (!state.statusHandle) {
    return;
  }

  ReportStatus(state.statusHandle, SERVICE_START_PENDING, NO_ERROR, 3000);
  TryLoadCore(state);
  ReportStatus(state.statusHandle, SERVICE_RUNNING);

  RunPipeServer(state.stopSource.get_token(), state);
  StopCoreOnShutdown(state);

  ReportStatus(state.statusHandle, SERVICE_STOPPED);
}

void InstallService() {
  wil::unique_schandle scm(
      OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
  THROW_LAST_ERROR_IF(!scm);

  wchar_t path[MAX_PATH]{};
  THROW_LAST_ERROR_IF(GetModuleFileNameW(nullptr, path, MAX_PATH) == 0);
  const std::wstring quotedPath = L"\"" + std::wstring(path) + L"\"";

  wil::unique_schandle service(CreateServiceW(
      scm.get(), kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
      SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
      quotedPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  THROW_LAST_ERROR_IF(!service);

  std::wcout << L"Служба " << kServiceName << L" установлена.\n";
}

void UninstallService() {
  wil::unique_schandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  THROW_LAST_ERROR_IF(!scm);

  wil::unique_schandle service(
      OpenServiceW(scm.get(), kServiceName, DELETE));
  THROW_LAST_ERROR_IF(!service);
  THROW_LAST_ERROR_IF(!DeleteService(service.get()));

  std::wcout << L"Служба " << kServiceName << L" удалена.\n";
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
  if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
    ServiceState::Instance().stopSource.request_stop();
    return TRUE;
  }
  return FALSE;
}

// Режим для разработки: гоняет тот же pipe-сервер в консоли, без установки
// службы и без SCM control handler.
void RunInConsole() {
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
  auto& state = ServiceState::Instance();
  TryLoadCore(state);
  std::wcout << L"sovereign core: pipe " << sovereign::ipc::kPipeName
             << L", core " << (state.core ? L"loaded" : L"NOT loaded")
             << L", Ctrl+C для остановки.\n";
  RunPipeServer(state.stopSource.get_token(), state);
  StopCoreOnShutdown(state);
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  const std::wstring arg = (argc > 1) ? argv[1] : L"";

  try {
    if (arg == L"--install") {
      InstallService();
      return 0;
    }
    if (arg == L"--uninstall") {
      UninstallService();
      return 0;
    }
    if (arg == L"--run") {
      RunInConsole();
      return 0;
    }
  } catch (const wil::ResultException& e) {
    std::wcerr << L"Ошибка: " << e.what() << L"\n";
    return 1;
  }

  const SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<LPWSTR>(kServiceName), ServiceMain}, {nullptr, nullptr}};
  if (!StartServiceCtrlDispatcherW(table)) {
    std::wcerr << L"Запускать без параметров можно только под SCM. "
                   L"Используйте --run для консоли.\n";
    return 1;
  }
  return 0;
}
