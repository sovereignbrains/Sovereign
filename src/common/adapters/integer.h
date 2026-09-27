#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>

// What sing's JSON unmarshaler (Go's encoding/json) puts into an integer
// field: a JSON integer literal that fits the type - nothing else. 70000 into
// a uint16 port, 1.5, 1e2, 443.0, "443", true, and any minus sign (even "-0")
// into an unsigned field are errors; null leaves the zero value. nlohmann's
// get<> converts all of those silently (70000 -> 4464), so generated code
// reads its integer fields through here. The rules are checked against the
// real unmarshaler: tests/golden/fixtures/integers.golden.json.

namespace sovereign::adapters {

template <typename T>
T GetInteger(const nlohmann::json& j) {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
  if (j.is_null()) {
    return T{};
  }
  // nlohmann keeps a literal without a minus sign as unsigned, one with a
  // minus sign as signed, anything with a fraction or exponent as a float.
  if (j.is_number_unsigned()) {
    const auto value = j.get<std::uint64_t>();
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
      return static_cast<T>(value);
    }
  } else if (j.is_number_integer() && std::is_signed_v<T>) {
    const auto value = j.get<std::int64_t>();
    if (value >= static_cast<std::int64_t>(std::numeric_limits<T>::min()) &&
        value <= static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
      return static_cast<T>(value);
    }
  }
  throw std::invalid_argument("json: cannot unmarshal " + j.dump() + " into an integer field of " +
                              std::to_string(sizeof(T) * 8) + (std::is_signed_v<T> ? " signed" : " unsigned") +
                              " bits");
}

}  // namespace sovereign::adapters
