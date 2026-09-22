#pragma once

#include <cstdint>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

// Mirrors github.com/sagernet/sing-box/option.FwMark: a uint32 marshaled
// as a lowercase "0x..." hex string (option/tun.go). Unmarshal there also
// accepts a bare JSON number for backward compatibility, so we do too —
// but to_json always emits the string form, matching what Go's own
// Marshal actually produces (the bare-number path is unmarshal-only
// leniency, never an output shape).

namespace sovereign::adapters {

class FwMark {
 public:
  FwMark() = default;
  explicit FwMark(std::uint32_t value) : value_(value) {}

  std::uint32_t value() const { return value_; }

 private:
  std::uint32_t value_ = 0;
};

inline bool IsEmptyValue(const FwMark& f) { return f.value() == 0; }

inline void to_json(nlohmann::json& j, const FwMark& f) {
  std::ostringstream out;
  out << "0x" << std::hex << f.value();
  j = out.str();
}

inline void from_json(const nlohmann::json& j, FwMark& f) {
  if (j.is_number_unsigned()) {
    f = FwMark(j.get<std::uint32_t>());
    return;
  }
  const std::string text = j.get<std::string>();
  f = FwMark(static_cast<std::uint32_t>(std::stoul(text, nullptr, 0)));
}

}  // namespace sovereign::adapters
