#pragma once

#include <optional>
#include <string>
#include <string_view>

// The service's box_exitip answer (gocore/exitip.go): the address the
// internet sees through the server in use, and its country. Apart from the
// worker so it's unit-tested (tests/unit/exit_ip_test.cpp) and fuzzed.

namespace sovereign::tray {

struct ExitIp {
  std::string ip;       // "185.12.34.56" / "2a01:4f8::1"; empty until known
  std::string country;  // "NL": two uppercase letters, or empty
  bool pending = false; // a lookup runs
  std::string error;    // why the last one failed
};

// nullopt unless it's a box_exitip answer. Fields that don't look like what
// they should (an address of other characters, a country that isn't two
// letters) are left empty: they're drawn in the window.
std::optional<ExitIp> ParseExitIpResponse(std::string_view response);

}  // namespace sovereign::tray
