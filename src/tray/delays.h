#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace sovereign::tray {

// One server's latency as the service's box_delays reports it.
struct Delay {
  enum class State : std::uint8_t { Pending, Ok, Failed };
  State state = State::Pending;
  int ms = 0;         // Ok
  std::string error;  // Failed
};

// Results by outbound tag; nullopt if `response` isn't a box_delays answer
// (an error, or garbage). Entries that aren't a result are skipped.
std::optional<std::map<std::string, Delay>> ParseDelaysResponse(std::string_view response);

}  // namespace sovereign::tray
