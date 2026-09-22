// Golden test: the fixture is real sing-box output (see
// tools/goldengen, tests/golden/fixtures/anytls_outbound.golden.json,
// regenerate with `go run ./tools/goldengen -fixture anytls_outbound`).
// We unmarshal it through our codegen'd type + envelope adapter and
// re-marshal it; a structural JSON diff (key order doesn't matter for a
// config object, unlike the P3 wire-format byte diffs) must be empty.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "adapters/outbound_envelope.h"
#include "anytls_outbound.gen.h"

namespace {

using AnyTLSOutbound =
    sovereign::adapters::Outbound<sovereign::codegen::option::AnyTLSOutboundOptions>;

nlohmann::json LoadFixture(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("cannot open fixture: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return nlohmann::json::parse(buffer.str());
}

}  // namespace

// bugprone-exception-escape can't see through the try/catch(...) below and
// prove every path is covered; it is a documented limitation of the check,
// not a real gap here.
int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <fixture.json>\n";
    return 2;
  }

  try {
    const nlohmann::json golden = LoadFixture(argv[1]);

    AnyTLSOutbound outbound = golden.get<AnyTLSOutbound>();
    const nlohmann::json roundTripped = outbound;

    if (roundTripped != golden) {
      std::cerr << "golden mismatch\n  expected: " << golden.dump()
                << "\n  actual:   " << roundTripped.dump() << "\n";
      return 1;
    }

    std::cout << "OK: " << roundTripped.dump() << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "error: unknown exception\n";
    return 1;
  }
}
