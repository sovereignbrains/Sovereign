#include "pipe_client.h"

#include <windows.h>

#include <wil/resource.h>

#include <array>

#include "protocol.h"

namespace sovereign::tray {

std::optional<std::string> RequestService(const std::string& json) {
  // A busy instance means the service is mid-request with another client;
  // wait briefly for the next one rather than report the service as down.
  constexpr DWORD kBusyWaitMs = 2000;

  wil::unique_hfile pipe;
  for (int attempt = 0; attempt < 2; ++attempt) {
    pipe.reset(CreateFileW(ipc::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (pipe || GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(ipc::kPipeName, kBusyWaitMs)) {
      break;
    }
  }
  if (!pipe) {
    return std::nullopt;
  }

  // The service writes one message per response; message read mode lets a
  // response larger than one read arrive as ERROR_MORE_DATA chunks.
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) {
    return std::nullopt;
  }

  DWORD written = 0;
  if (!WriteFile(pipe.get(), json.data(), static_cast<DWORD>(json.size()), &written, nullptr) ||
      written != json.size()) {
    return std::nullopt;
  }

  std::string response;
  std::array<char, ipc::kPipeBufferSize> chunk{};
  for (;;) {
    DWORD read = 0;
    const BOOL ok = ReadFile(pipe.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr);
    response.append(chunk.data(), read);
    if (ok) {
      return response;
    }
    if (GetLastError() != ERROR_MORE_DATA || response.size() > ipc::kMaxMessageBytes) {
      return std::nullopt;
    }
  }
}

}  // namespace sovereign::tray
