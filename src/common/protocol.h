#pragma once

#include <windows.h>

#include <cstddef>

// P0: минимальный контракт control-plane pipe. Реальная схема команд
// (start/stop/stats) появится в P1 вместе с кодоген option-structs.

namespace sovereign::ipc {

inline constexpr wchar_t kPipeName[] = LR"(\\.\pipe\sovereign-control)";
inline constexpr DWORD kPipeBufferSize = 4096;

// Upper bound for one message. kPipeBufferSize is only the read chunk: a full
// sing-box config with several outbounds and inline rules is tens of KB and
// arrives in pieces. This cap only stops a runaway client from growing the
// service's memory without limit.
inline constexpr std::size_t kMaxMessageBytes = 4 * 1024 * 1024;

}  // namespace sovereign::ipc
