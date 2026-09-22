#pragma once

#include <windows.h>

// P0: минимальный контракт control-plane pipe. Реальная схема команд
// (start/stop/stats) появится в P1 вместе с кодоген option-structs.

namespace sovereign::ipc {

inline constexpr wchar_t kPipeName[] = LR"(\\.\pipe\sovereign-control)";
inline constexpr DWORD kPipeBufferSize = 4096;

}  // namespace sovereign::ipc
