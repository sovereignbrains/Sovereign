// libFuzzer target: ParseClientHello on arbitrary bytes, plus the properties
// the conformance harness relies on - a record that parses serializes back to
// exactly its input, canonicalization never fails, and a canonical hello has
// no differences from itself.

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
  for (const bool randomized : {false, true}) {
    const Canonical canonical = Canonicalize(*hello, CanonicalOptions{.extensionOrderRandomized = randomized});
    if (!Diff(canonical.hello, canonical.hello).empty()) {
      std::abort();
    }
  }
  return 0;
}
