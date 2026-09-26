#pragma once

#include <optional>
#include <string>

namespace sovereign::tray {

// One request/response round trip over the service's control pipe
// (sovereign::ipc::kPipeName). Blocking - call it off the UI thread.
// nullopt means the service isn't reachable (not installed, stopped, or the
// pipe broke mid-request); anything the service answered, including an
// {"cmd":"error"} response, comes back as text.
std::optional<std::string> RequestService(const std::string& json);

}  // namespace sovereign::tray
