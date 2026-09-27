// ClientHello parsing and canonicalization (src/conformance/client_hello.h)
// against real captures of the pinned reference sing-box: argv[1] is the
// fixtures directory (tools/conformance/capture.py wrote it).

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "client_hello.h"

namespace {

using namespace sovereign::conformance;
using Bytes = std::vector<std::uint8_t>;

constexpr int kRuns = 5;
std::filesystem::path g_fixtures;

Bytes FromHex(std::string_view hex) {
  Bytes out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

std::vector<Bytes> Captures(std::string_view name) {
  std::vector<Bytes> out;
  for (int n = 1; n <= kRuns; ++n) {
    std::ifstream file(g_fixtures / (std::string(name) + "." + std::to_string(n) + ".hex"));
    std::string hex;
    file >> hex;
    out.push_back(FromHex(hex));
  }
  return out;
}

const std::vector<Bytes>& Reality() {
  static const std::vector<Bytes> captures = Captures("anytls-reality-chrome");
  return captures;
}

const std::vector<Bytes>& Tls() {
  static const std::vector<Bytes> captures = Captures("anytls-tls-chrome");
  return captures;
}

constexpr CanonicalOptions kChrome{.extensionOrderRandomized = true};

// Canonical form of a capture; a parse failure fails the check and yields an
// empty hello (which then matches nothing real).
Canonical CanonicalOf(const Bytes& record, const CanonicalOptions& options = kChrome) {
  const auto parsed = ParseClientHello(record);
  CHECK(parsed.has_value());
  return parsed ? Canonicalize(*parsed, options) : Canonical{};
}

bool Mentions(const std::vector<std::string>& lines, std::string_view what) {
  for (const std::string& line : lines) {
    if (line.find(what) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void TestRoundTrip() {
  for (const auto* set : {&Reality(), &Tls()}) {
    for (const Bytes& record : *set) {
      CHECK(!record.empty());
      const auto parsed = ParseClientHello(record);
      CHECK(parsed.has_value());
      if (parsed) {
        CHECK(Serialize(*parsed) == record);
      }
    }
  }
}

// The self-test of the mask list: five connections of the same reference
// client, every one of them different on the wire, are one canonical hello.
void TestReferenceAgreesWithItself() {
  for (const auto* set : {&Reality(), &Tls()}) {
    const Canonical first = CanonicalOf((*set)[0]);
    CHECK(first.violations.empty());
    for (const Bytes& record : *set) {
      if (&record != &(*set)[0]) {
        CHECK(record != (*set)[0]);  // on the wire every connection differs
      }
      const Canonical other = CanonicalOf(record);
      CHECK(other.violations.empty());
      CHECK(Diff(first.hello, other.hello).empty());
      CHECK(Serialize(first.hello) == Serialize(other.hello));
    }
  }
}

// Without the permutation allowance the order differences are real and
// reported - the option is needed for chrome, and strict mode would catch it.
void TestOrderIsComparedUnlessRandomized() {
  const CanonicalOptions strict{};
  bool anyOrderDifference = false;
  const Canonical first = CanonicalOf(Reality()[0], strict);
  for (const Bytes& record : Reality()) {
    const auto diff = Diff(first.hello, CanonicalOf(record, strict).hello);
    anyOrderDifference = anyOrderDifference || Mentions(diff, "extension order");
  }
  CHECK(anyOrderDifference);
}

// REALITY and plain TLS with the same fingerprint differ where they really do.
void TestRealityVersusTls() {
  const auto diff = Diff(CanonicalOf(Tls()[0]).hello, CanonicalOf(Reality()[0]).hello);
  CHECK(!diff.empty());
  CHECK(Mentions(diff, "supported_groups"));
  CHECK(Mentions(diff, "key_share"));
}

// Flip every byte of a capture, one at a time. The flips that go unnoticed
// must be exactly the masked bytes - no more (a stable field would be
// unprotected), no fewer (a mask would be missing).
void TestEveryUnmaskedByteIsCompared() {
  const Bytes& record = Reality()[0];
  const auto parsed = ParseClientHello(record);
  CHECK(parsed.has_value());
  if (!parsed) {
    return;
  }
  // Masked bytes: random, session id, key share keys, ECH config id + enc + payload.
  std::size_t masked = parsed->random.size() + parsed->sessionId.size();
  for (const Extension& e : parsed->extensions) {
    if (e.type == 51) {
      std::size_t i = 2;
      while (i + 4 <= e.data.size()) {
        const std::size_t length = (static_cast<std::size_t>(e.data[i + 2]) << 8U) | e.data[i + 3];
        masked += length;
        i += 4 + length;
      }
    } else if (e.type == 0xFE0D) {
      const std::size_t encLength = (static_cast<std::size_t>(e.data[6]) << 8U) | e.data[7];
      const std::size_t payloadAt = 8 + encLength;
      const std::size_t payloadLength = (static_cast<std::size_t>(e.data[payloadAt]) << 8U) | e.data[payloadAt + 1];
      masked += 1 + encLength + payloadLength;
    }
  }
  const Canonical reference = CanonicalOf(Reality()[1]);
  std::size_t unnoticed = 0;
  for (std::size_t i = 0; i < record.size(); ++i) {
    Bytes mutated = record;
    mutated[i] ^= 0x01U;
    const auto hello = ParseClientHello(mutated);
    if (!hello) {
      continue;  // rejected outright: noticed
    }
    const Canonical canonical = Canonicalize(*hello, kChrome);
    if (canonical.violations.empty() && Serialize(canonical.hello) == Serialize(reference.hello)) {
      ++unnoticed;
    }
  }
  std::cout << "mutation: " << unnoticed << " of " << record.size() << " single-byte flips unnoticed, "
            << masked << " bytes masked\n";
  CHECK(unnoticed == masked);
}

void TestTruncationIsRejected() {
  const Bytes& record = Reality()[0];
  for (std::size_t n = 0; n < record.size(); ++n) {
    CHECK(!ParseClientHello(std::span(record).first(n)).has_value());
  }
  Bytes longer = record;
  longer.push_back(0);
  CHECK(!ParseClientHello(longer).has_value());
}

void TestEchPayloadRule() {
  auto parsed = ParseClientHello(Reality()[0]);
  CHECK(parsed.has_value());
  if (!parsed) {
    return;
  }
  for (Extension& e : parsed->extensions) {
    if (e.type == 0xFE0D) {
      // Rebuild with a 150-byte payload: not 32-byte padding plus a 16-byte tag.
      const std::size_t encLength = (static_cast<std::size_t>(e.data[6]) << 8U) | e.data[7];
      e.data.resize(8 + encLength);
      e.data.push_back(0);
      e.data.push_back(150);
      e.data.resize(e.data.size() + 150, 0xAB);
    }
  }
  const Canonical canonical = Canonicalize(*parsed, kChrome);
  CHECK(Mentions(canonical.violations, "encrypted_client_hello"));
}

void TestGrease() {
  for (unsigned k = 0; k < 16; ++k) {
    CHECK(IsGrease(static_cast<std::uint16_t>(0x0A0AU + 0x1010U * k)));
  }
  CHECK(!IsGrease(0x0A1A));
  CHECK(!IsGrease(0x1301));
  CHECK(!IsGrease(0x0000));
  CHECK(ExtensionName(0x3A3A) == "GREASE");
  CHECK(ExtensionName(51) == "key_share");
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    if (argc < 2) {
      std::cerr << "usage: conformance-client-hello <fixtures dir>\n";
      return 2;
    }
    g_fixtures = argv[1];
    TestRoundTrip();
    TestReferenceAgreesWithItself();
    TestOrderIsComparedUnlessRandomized();
    TestRealityVersusTls();
    TestEveryUnmaskedByteIsCompared();
    TestTruncationIsRejected();
    TestEchPayloadRule();
    TestGrease();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
