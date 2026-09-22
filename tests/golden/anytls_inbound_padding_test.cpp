// Golden test for badoption.Listable[T]'s two wire shapes (see
// tools/goldengen's anytls_inbound_padding_single/_array fixtures): a
// single-element list marshals as the bare element, any other length as a
// JSON array. This binary is invoked twice by CTest, once per fixture, to
// cover both directions.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "anytls_inbound.gen.h"

namespace {

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

// NOLINTNEXTLINE(bugprone-exception-escape) - see anytls_outbound_test.cpp
int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <fixture.json>\n";
    return 2;
  }

  try {
    const nlohmann::json golden = LoadFixture(argv[1]);

    sovereign::codegen::option::AnyTLSInboundOptions opts = golden.get<sovereign::codegen::option::AnyTLSInboundOptions>();
    const nlohmann::json roundTripped = opts;

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
