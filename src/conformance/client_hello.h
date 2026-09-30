#pragma once

// A TLS ClientHello as a client sends it in its first record, and the
// canonical form the conformance harness compares: the fields that differ
// between two connections of the *same* client by design - RFC 8446 randoms,
// RFC 8701 GREASE, fresh key shares, Chrome's per-connection extension
// permutation, ECH's per-connection values - replaced by placeholders of the
// same shape, so that every other byte can be compared exactly. Each
// normalization is listed in client_hello.cpp with its reason; nothing else is
// touched, and two captures of the reference client canonicalizing equal is
// the self-test that the list is complete (tests/conformance).

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace sovereign::conformance {

struct Extension {
  std::uint16_t type = 0;
  std::vector<std::uint8_t> data;

  friend bool operator==(const Extension&, const Extension&) = default;
};

struct ClientHello {
  std::uint16_t recordVersion = 0;  // the record layer's legacy_record_version
  std::uint16_t legacyVersion = 0;
  std::array<std::uint8_t, 32> random{};
  std::vector<std::uint8_t> sessionId;
  std::vector<std::uint16_t> cipherSuites;
  std::vector<std::uint8_t> compressionMethods;
  std::vector<Extension> extensions;  // in wire order

  friend bool operator==(const ClientHello&, const ClientHello&) = default;
};

// The first record of a connection -> its ClientHello. A client's first flight
// is one handshake record holding one whole ClientHello; anything else
// (another record type, a fragmented or truncated message, trailing bytes,
// a repeated extension - RFC 8446 4.2) is an error, not a guess.
std::expected<ClientHello, std::string> ParseClientHello(std::span<const std::uint8_t> record);

// The record back, every length recomputed. Serialize(Parse(r)) == r.
std::vector<std::uint8_t> Serialize(const ClientHello& hello);

struct CanonicalOptions {
  // Chrome permutes its extensions on every connection (since Chrome 106,
  // uTLS follows); a client that doesn't must keep its order, so this is off
  // unless the fingerprint under test is known to shuffle.
  bool extensionOrderRandomized = false;
  // GREASE ECH picks per connection what the fingerprint allows (uTLS's
  // GREASEEncryptedClientHelloExtension, its Candidate* fields in
  // u_parrots.go). Empty: BoringSSL's - Chrome's - rule, the payload a
  // 32-byte-padded plaintext plus a 16-byte tag and one AEAD, compared as
  // is. Otherwise the payload's length (tag included) must be one of
  // echPayloadLengths and the AEAD one of echAeads; both are then masked.
  std::span<const std::size_t> echPayloadLengths{};
  std::span<const std::uint16_t> echAeads{};
};

struct Canonical {
  ClientHello hello;
  // Shape rules a per-connection field broke - its bytes are masked, so this
  // is the only place a malformed one would show.
  std::vector<std::string> violations;
};

Canonical Canonicalize(const ClientHello& hello, const CanonicalOptions& options);

// What differs between two (canonical) ClientHellos, one readable line per
// difference; empty means identical.
std::vector<std::string> Diff(const ClientHello& reference, const ClientHello& ours);

// RFC 8701: 0x0A0A, 0x1A1A, ... 0xFAFA.
bool IsGrease(std::uint16_t value);

std::string ExtensionName(std::uint16_t type);

}  // namespace sovereign::conformance
