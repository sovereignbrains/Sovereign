// libFuzzer target: ParseClientHello on arbitrary bytes, plus the properties
// the conformance harness relies on - a record that parses serializes back to
// exactly its input, canonicalization never fails, and a canonical hello has
// no differences from itself.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "client_hello.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace sovereign::conformance;
  const std::span<const std::uint8_t> input(data, size);
  const auto hello = ParseClientHello(input);
  if (!hello) {
    return 0;
  }
  if (Serialize(*hello) != std::vector<std::uint8_t>(input.begin(), input.end())) {
    std::abort();  // the parse lost or invented something
  }
  // Chrome's shape (shuffled, BoringSSL's ECH rule), a fixed order, and
  // Firefox's (its own GREASE ECH candidates, as in harness.cpp).
  static constexpr std::array<std::size_t, 1> kEchPayloads{239};
  static constexpr std::array<std::uint16_t, 2> kEchAeads{0x0001, 0x0003};
  for (const CanonicalOptions& options :
       {CanonicalOptions{.extensionOrderRandomized = true}, CanonicalOptions{},
        CanonicalOptions{.echPayloadLengths = kEchPayloads, .echAeads = kEchAeads}}) {
    const Canonical canonical = Canonicalize(*hello, options);
    if (!Diff(canonical.hello, canonical.hello).empty()) {
      std::abort();
    }
  }
  return 0;
}
