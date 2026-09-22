#include "pipe_server.h"

#include <wil/resource.h>
#include <wil/result.h>

#include <array>

#include "protocol.h"

namespace sovereign::service {

namespace {

wil::unique_hfile CreatePipeInstance(const std::wstring& pipeName) {
  wil::unique_hfile pipe(CreateNamedPipeW(
      pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
      1,  // nMaxInstances: один инстанс, обрабатываем клиентов по очереди
      sovereign::ipc::kPipeBufferSize, sovereign::ipc::kPipeBufferSize, 0,
      nullptr));
  THROW_LAST_ERROR_IF(!pipe);
  return pipe;
}

// Ждёт подключения клиента; возвращает false, если сервер должен остановиться.
bool WaitForClient(HANDLE pipe, HANDLE stopEvent) {
  OVERLAPPED overlapped{};
  wil::unique_event_nothrow connectEvent;
  THROW_IF_FAILED(connectEvent.create(wil::EventOptions::ManualReset));
  overlapped.hEvent = connectEvent.get();

  if (ConnectNamedPipe(pipe, &overlapped)) {
    return true;  // клиент успел подключиться синхронно
  }

  const DWORD error = GetLastError();
  if (error == ERROR_PIPE_CONNECTED) {
    return true;
  }
  THROW_LAST_ERROR_IF(error != ERROR_IO_PENDING);

  const std::array<HANDLE, 2> waitHandles{stopEvent, connectEvent.get()};
  const DWORD waitResult =
      WaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()),
                              waitHandles.data(), FALSE, INFINITE);
  if (waitResult == WAIT_OBJECT_0) {
    CancelIoEx(pipe, &overlapped);
    return false;
  }

  DWORD bytesTransferred = 0;
  THROW_LAST_ERROR_IF(
      !GetOverlappedResult(pipe, &overlapped, &bytesTransferred, FALSE));
  return true;
}

std::string ReadMessage(HANDLE pipe) {
  std::array<char, sovereign::ipc::kPipeBufferSize> buffer{};
  OVERLAPPED overlapped{};
  wil::unique_event_nothrow ioEvent;
  THROW_IF_FAILED(ioEvent.create(wil::EventOptions::ManualReset));
  overlapped.hEvent = ioEvent.get();

  const BOOL immediate = ReadFile(pipe, buffer.data(),
                                   static_cast<DWORD>(buffer.size()), nullptr,
                                   &overlapped);
  DWORD bytesRead = 0;
  if (!immediate) {
    THROW_LAST_ERROR_IF(GetLastError() != ERROR_IO_PENDING);
  }
  THROW_LAST_ERROR_IF(!GetOverlappedResult(pipe, &overlapped, &bytesRead, TRUE));
  return std::string(buffer.data(), bytesRead);
}

void WriteMessage(HANDLE pipe, const std::string& message) {
  OVERLAPPED overlapped{};
  wil::unique_event_nothrow ioEvent;
  THROW_IF_FAILED(ioEvent.create(wil::EventOptions::ManualReset));
  overlapped.hEvent = ioEvent.get();

  const BOOL immediate =
      WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()),
                nullptr, &overlapped);
  DWORD bytesWritten = 0;
  if (!immediate) {
    THROW_LAST_ERROR_IF(GetLastError() != ERROR_IO_PENDING);
  }
  THROW_LAST_ERROR_IF(
      !GetOverlappedResult(pipe, &overlapped, &bytesWritten, TRUE));
}

}  // namespace

PipeServer::PipeServer(std::wstring pipeName, RequestHandler handler)
    : pipeName_(std::move(pipeName)), handler_(std::move(handler)) {}

void PipeServer::Run(std::stop_token stopToken) {
  wil::unique_event_nothrow stopEvent;
  THROW_IF_FAILED(stopEvent.create(wil::EventOptions::ManualReset));

  std::stop_callback signalStop(stopToken,
                                 [&] { SetEvent(stopEvent.get()); });

  wil::unique_hfile pipe = CreatePipeInstance(pipeName_);

  while (!stopToken.stop_requested()) {
    if (!WaitForClient(pipe.get(), stopEvent.get())) {
      break;
    }

    try {
      const std::string request = ReadMessage(pipe.get());
      const std::string response = handler_(request);
      WriteMessage(pipe.get(), response);
      // WriteFile-overlapped завершается, как только данные попали в буфер
      // пайпа, а не когда клиент их забрал. Без этого FlushFileBuffers
      // DisconnectNamedPipe ниже может оборвать соединение раньше, чем
      // клиент успеет прочитать ответ (клиент увидит EOF/0 байт).
      FlushFileBuffers(pipe.get());
    } catch (...) {
      // Один клиент не должен ронять сервер целиком; соединение просто
      // закрывается ниже через DisconnectNamedPipe.
    }

    DisconnectNamedPipe(pipe.get());
  }
}

}  // namespace sovereign::service
