#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// Mirrors github.com/sagernet/sing-box/option.CurvePreference (option/tls.go):
// a TLS curve ID (crypto/tls.CurveID values) marshaled as its name - P256,
// P384, P521, X25519, X25519MLKEM768 - and parsed case-insensitively. Any
// other name or ID is an error in both directions, as it is in Go.

namespace sovereign::adapters {

enum class CurvePreference : std::uint16_t {
  P256 = 23,
  P384 = 24,
  P521 = 25,
  X25519 = 29,
  X25519MLKEM768 = 4588,
};

inline std::string_view CurvePreferenceName(CurvePreference curve) {
  switch (curve) {
    case CurvePreference::P256: return "P256";
    case CurvePreference::P384: return "P384";
    case CurvePreference::P521: return "P521";
    case CurvePreference::X25519: return "X25519";
    case CurvePreference::X25519MLKEM768: return "X25519MLKEM768";
  }
  throw std::invalid_argument("unknown curve id: " + std::to_string(static_cast<int>(curve)));
}

inline void to_json(nlohmann::json& j, const CurvePreference& curve) { j = std::string(CurvePreferenceName(curve)); }

inline void from_json(const nlohmann::json& j, CurvePreference& curve) {
  const auto name = j.get<std::string>();
  std::string upper;
  for (const char c : name) {
    upper += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  for (const CurvePreference candidate : {CurvePreference::P256, CurvePreference::P384, CurvePreference::P521,
                                          CurvePreference::X25519, CurvePreference::X25519MLKEM768}) {
    if (upper == CurvePreferenceName(candidate)) {
      curve = candidate;
      return;
    }
  }
  throw std::invalid_argument("unknown curve name: " + name);
}

}  // namespace sovereign::adapters
