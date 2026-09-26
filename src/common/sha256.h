#pragma once

// SHA-256 as lowercase hex, via Windows CNG (bcrypt) - no crypto dependency.
// Used to name a config without carrying it: the service reports the hash of
// the config its box runs (box_stats "config_sha256"), the tray compares it
// with the hash of its own config.json. Both sides hash the same text - the
// config as nlohmann::json::dump() writes it - so equal configs give equal
// hashes. Links bcrypt.lib (#pragma below).

// This header brings windows.h into files that never had it (control.cpp):
// without NOMINMAX its min/max macros break std::max there - which the CI's
// clang-tidy hit (2fdc41b) even though the build defines NOMINMAX globally.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <wil/result.h>

#include <array>
#include <string>
#include <string_view>

#pragma comment(lib, "bcrypt.lib")

namespace sovereign {

inline std::string Sha256Hex(std::string_view data) {
  std::array<unsigned char, 32> digest{};
  THROW_IF_NTSTATUS_FAILED(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                      reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                      static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size())));
  constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (const unsigned char byte : digest) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 0x0F]);
  }
  return hex;
}

}  // namespace sovereign
