#pragma once

#include <array>
#include <cstdint>

// X25519 (RFC 7748): a WireGuard key pair for the WARP account (warp.h) -
// the private key never leaves the machine, Cloudflare gets the public one.
// The field arithmetic is TweetNaCl's, constant-time; checked against the
// RFC's vectors in tests/unit/warp_test.cpp.

namespace sovereign::tray {

using X25519Key = std::array<std::uint8_t, 32>;

// scalar * point (u-coordinate), the scalar clamped as X25519 does.
X25519Key X25519(const X25519Key& scalar, const X25519Key& point);

// The public key of a private one: scalar * 9.
X25519Key X25519Public(const X25519Key& privateKey);

}  // namespace sovereign::tray
