#pragma once

#include <windows.h>

#include <functional>
#include <stop_token>
#include <string>

namespace sovereign::service {

// Однопоточный echo-сервер именованного канала. Принимает один запрос за
// подключение, отвечает через handler и закрывает соединение. Приём нового
// клиента (ConnectNamedPipe) выполняется через overlapped I/O, чтобы
// std::stop_token мог прервать ожидание — без этого служба не смогла бы
// остановиться, пока не подключится клиент.
class PipeServer {
 public:
  using RequestHandler = std::function<std::string(const std::string& request)>;

  explicit PipeServer(std::wstring pipeName, RequestHandler handler);

  // Блокирует вызывающий поток до stopToken.stop_requested().
  void Run(const std::stop_token& stopToken);

 private:
  std::wstring pipeName_;
  RequestHandler handler_;
};

}  // namespace sovereign::service
