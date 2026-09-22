#include <windows.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stop_token>
#include <string>

#include "go_core.h"
#include "pipe_server.h"
#include "protocol.h"

namespace {

constexpr wchar_t kServiceName[] = L"SovereignCore";
constexpr wchar_t kServiceDisplayName[] = L"Sovereign Core";

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
  std::optional<sovereign::service::GoCore> goCore;
};

// GoCore is optional at this stage (P2 atom 1a proves the happy path); if
// the DLL is missing or fails to load, the service still starts and the
// pipe still answers everything except box_ping — a missing dev-time
// artifact shouldn't take down the whole service.
void TryLoadGoCore(ServiceState& state) {
  try {
    state.goCore.emplace(sovereign::service::ResolveGoCoreDllPath());
  } catch (const wil::ResultException& e) {
    std::wcerr << L"GoCore недоступен: " << e.what() << L"\n";
  }
}

std::string HandlePipeRequest(const std::string& request,
                               sovereign::service::GoCore* goCore) {
  // P0: эхо с подтверждением, что процесс — служба. P1 добавит реальные
  // команды (start/stop/stats) поверх кодоген-схемы конфига.
  nlohmann::json response;
  try {
    const auto parsed = nlohmann::json::parse(request);
    const std::string cmd = parsed.value("cmd", "");
    if (cmd == "box_ping") {
      if (goCore == nullptr) {
        response["cmd"] = "error";
        response["message"] = "gocore not loaded";
      } else {
        response["cmd"] = "box_pong";
        response["reply"] = goCore->Ping();
      }
    } else {
      response["cmd"] = "pong";
      response["echo"] = parsed;
    }
  } catch (const nlohmann::json::parse_error&) {
    response["cmd"] = "error";
    response["message"] = "invalid json";
  }
  return response.dump();
}

void ReportStatus(SERVICE_STATUS_HANDLE handle, DWORD state,
                   DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
  SERVICE_STATUS status{};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwControlsAccepted =
      (state == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP;
  status.dwWin32ExitCode = exitCode;
  status.dwWaitHint = waitHint;
  SetServiceStatus(handle, &status);
}

DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD, LPVOID, LPVOID) {
  auto& state = ServiceState::Instance();
  switch (control) {
    case SERVICE_CONTROL_STOP:
      ReportStatus(state.statusHandle, SERVICE_STOP_PENDING, NO_ERROR, 3000);
      state.stopSource.request_stop();
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

void RunPipeServer(const std::stop_token& stopToken, sovereign::service::GoCore* goCore) {
  sovereign::service::PipeServer server(
      sovereign::ipc::kPipeName,
      [goCore](const std::string& request) { return HandlePipeRequest(request, goCore); });
  server.Run(stopToken);
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
  auto& state = ServiceState::Instance();
  state.statusHandle =
      RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandler, nullptr);
  if (!state.statusHandle) {
    return;
  }

  ReportStatus(state.statusHandle, SERVICE_START_PENDING, NO_ERROR, 3000);
  TryLoadGoCore(state);
  ReportStatus(state.statusHandle, SERVICE_RUNNING);

  RunPipeServer(state.stopSource.get_token(), state.goCore ? &*state.goCore : nullptr);

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
  TryLoadGoCore(state);
  std::wcout << L"sovereign core: pipe " << sovereign::ipc::kPipeName
             << L", GoCore " << (state.goCore ? L"loaded" : L"NOT loaded")
             << L", Ctrl+C для остановки.\n";
  RunPipeServer(state.stopSource.get_token(), state.goCore ? &*state.goCore : nullptr);
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
