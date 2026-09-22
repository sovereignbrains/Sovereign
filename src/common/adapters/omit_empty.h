#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

// Mirrors Go's encoding/json isEmptyValue check, which `omitempty` tags
// rely on: false/0/""/nil are empty, everything else isn't. Structs are
// deliberately not covered (Go's isEmptyValue never treats a struct as
// empty either) — our codegen represents anything struct-shaped it can't
// safely flatten as nlohmann::json (default value `null`) or
// std::optional<T> (pointer fields), both handled below.

namespace sovereign::adapters {

inline bool IsEmptyValue(const std::string& v) { return v.empty(); }
inline bool IsEmptyValue(bool v) { return !v; }
inline bool IsEmptyValue(std::int8_t v) { return v == 0; }
inline bool IsEmptyValue(std::int16_t v) { return v == 0; }
inline bool IsEmptyValue(std::int32_t v) { return v == 0; }
inline bool IsEmptyValue(std::int64_t v) { return v == 0; }
inline bool IsEmptyValue(std::uint8_t v) { return v == 0; }
inline bool IsEmptyValue(std::uint16_t v) { return v == 0; }
inline bool IsEmptyValue(std::uint32_t v) { return v == 0; }
inline bool IsEmptyValue(std::uint64_t v) { return v == 0; }
inline bool IsEmptyValue(float v) { return v == 0; }
inline bool IsEmptyValue(double v) { return v == 0; }
inline bool IsEmptyValue(const nlohmann::json& v) { return v.is_null(); }

template <typename T>
bool IsEmptyValue(const std::optional<T>& v) {
  return !v.has_value();
}

}  // namespace sovereign::adapters
