#pragma once

// SHA-256 as lowercase hex. Used to name a config without carrying it: the
// service reports the hash of the config its box runs (box_stats
// "config_sha256"), the tray compares it with the hash of its own config.json.
// Both sides hash the same text - the config as nlohmann::json::dump() writes
// it - so equal configs give equal hashes.
//
// On Windows through CNG (bcrypt; links bcrypt.lib, #pragma below). Elsewhere
// - the TSan build of the control protocol on Linux (tests/tsan) - a plain
// FIPS 180-4 implementation; both are checked against the same known vectors
// (tests/unit/control_test.cpp, which the TSan build runs too).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#if defined(_WIN32)
// This header brings windows.h into files that never had it (control.cpp):
// without NOMINMAX its min/max macros break std::max there - which the CI's
// clang-tidy hit (2fdc41b) even though the build defines NOMINMAX globally.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <wil/result.h>

#pragma comment(lib, "bcrypt.lib")
#endif

namespace sovereign {

namespace sha256_detail {

inline std::string Hex(const std::array<unsigned char, 32>& digest) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (const unsigned char byte : digest) {
    hex.push_back(kHex[byte >> 4U]);
    hex.push_back(kHex[byte & 0x0FU]);
  }
  return hex;
}

#if !defined(_WIN32)
constexpr std::uint32_t Rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32U - n)); }

inline std::array<unsigned char, 32> Digest(std::string_view data) {
  constexpr std::array<std::uint32_t, 64> k{
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

  // The message, 0x80, zeroes up to 56 mod 64, then the bit length big-endian.
  std::string message(data);
  const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8U;
  message.push_back(static_cast<char>(0x80));
  while (message.size() % 64 != 56) {
    message.push_back('\0');
  }
  for (int i = 7; i >= 0; --i) {
    message.push_back(static_cast<char>((bits >> (8U * static_cast<unsigned>(i))) & 0xFFU));
  }

  for (std::size_t block = 0; block < message.size(); block += 64) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t t = 0; t < 16; ++t) {
      const auto byte = [&](std::size_t n) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(message[block + 4 * t + n]));
      };
      w[t] = (byte(0) << 24U) | (byte(1) << 16U) | (byte(2) << 8U) | byte(3);
    }
    for (std::size_t t = 16; t < 64; ++t) {
      const std::uint32_t s0 = Rotr(w[t - 15], 7) ^ Rotr(w[t - 15], 18) ^ (w[t - 15] >> 3U);
      const std::uint32_t s1 = Rotr(w[t - 2], 17) ^ Rotr(w[t - 2], 19) ^ (w[t - 2] >> 10U);
      w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, hh] = h;
    for (std::size_t t = 0; t < 64; ++t) {
      const std::uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[t] + w[t];
      const std::uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  std::array<unsigned char, 32> digest{};
  for (std::size_t i = 0; i < 8; ++i) {
    for (std::size_t j = 0; j < 4; ++j) {
      digest[4 * i + j] = static_cast<unsigned char>((h[i] >> (24U - 8U * j)) & 0xFFU);
    }
  }
  return digest;
}
#endif

}  // namespace sha256_detail

inline std::string Sha256Hex(std::string_view data) {
  std::array<unsigned char, 32> digest{};
#if defined(_WIN32)
  THROW_IF_NTSTATUS_FAILED(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                      reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                      static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size())));
#else
  digest = sha256_detail::Digest(data);
#endif
  return sha256_detail::Hex(digest);
}

}  // namespace sovereign
