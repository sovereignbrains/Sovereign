// libFuzzer target: the generated option structs and the handwritten adapters
// on arbitrary JSON. Bad input may be rejected (an exception from from_json -
// how nlohmann reports it), but whatever is accepted must round-trip: to_json,
// back through from_json without an error, and to_json again gives the same
// JSON. Durations are also fed as raw text: whatever ParseDuration accepts,
// FormatDuration must print as something that parses back to the same value.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

#include "adapters/duration.h"
#include "adapters/outbound_envelope.h"
#include "anytls_inbound.gen.h"
#include "anytls_outbound.gen.h"

namespace {

using AnyTLSOutbound = sovereign::adapters::Outbound<sovereign::codegen::option::AnyTLSOutboundOptions>;

template <typename T>
void RoundTrip(const nlohmann::json& input) {
  T first;
  try {
    first = input.get<T>();
  } catch (const std::exception&) {
    return;  // rejected: fine, that's what from_json does with bad input
  }
  const nlohmann::json once = first;
  const T second = once.get<T>();  // our own output must be accepted - no catch
  const nlohmann::json twice = second;
  if (once != twice) {
    std::abort();
  }
}

void DurationRoundTrip(const std::string& text) {
  std::chrono::nanoseconds value{};
  try {
    value = sovereign::adapters::ParseDuration(text);
  } catch (const std::invalid_argument&) {
    return;
  }
  if (sovereign::adapters::ParseDuration(sovereign::adapters::FormatDuration(value)) != value) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string text(reinterpret_cast<const char*>(data), size);
  DurationRoundTrip(text);
  const auto json = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (json.is_discarded()) {
    return 0;
  }
  RoundTrip<AnyTLSOutbound>(json);
  RoundTrip<sovereign::codegen::option::AnyTLSInboundOptions>(json);
  RoundTrip<sovereign::codegen::option::OutboundTLSOptions>(json);
  return 0;
}
