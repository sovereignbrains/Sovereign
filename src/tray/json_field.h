#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>

// Reading JSON the tray didn't write - a subscription from a server, the
// service's answers, config.json - without trusting its field types.
// nlohmann's value() throws type_error when a field exists with another type
// ({"tag": 5}), and the tray's worker thread has nowhere to catch it: one
// odd subscription took the tray down. Here a field of the wrong type reads
// as absent. Found by tests/fuzz/tray_input_fuzz.cpp.

namespace sovereign::tray {

template <typename T>
T Field(const nlohmann::json& object, const char* key, T fallback) {
  if (!object.is_object()) {
    return fallback;
  }
  const auto it = object.find(key);
  if (it == object.end()) {
    return fallback;
  }
  if constexpr (std::is_same_v<T, std::string>) {
    return it->is_string() ? it->template get<std::string>() : fallback;
  } else if constexpr (std::is_same_v<T, bool>) {
    return it->is_boolean() ? it->template get<bool>() : fallback;
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    return it->is_number_integer() ? it->template get<std::int64_t>() : fallback;
  } else {
    static_assert(std::is_same_v<T, void>, "Field<T>: add the type check for T");
  }
}

}  // namespace sovereign::tray
