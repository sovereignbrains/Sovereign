// Golden test: integer fields (adapters/integer.h) against what sing's JSON
// unmarshaler does with the same literals - tests/golden/fixtures/
// integers.golden.json, regenerate with `go run ./tools/goldengen -fixture
// integers`. Then the same rules end to end, through the generated AnyTLS
// outbound: a port of 70000 is rejected, not wrapped to 4464.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "adapters/integer.h"
#include "adapters/outbound_envelope.h"
#include "outbounds.gen.h"

namespace {

using AnyTLSOutbound = sovereign::adapters::Outbound<sovereign::codegen::option::AnyTLSOutboundOptions>;

nlohmann::json LoadFixture(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open fixture: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return nlohmann::json::parse(buffer.str());
}

// How many verdicts of one integer type we get wrong.
template <typename T>
int Check(const nlohmann::json& cases, const char* typeName) {
  int failures = 0;
  for (const auto& c : cases) {
    const auto text = c.at("text").get<std::string>();
    const bool goRejects = c.at("value").is_null();
    bool weReject = false;
    std::int64_t ours = 0;
    try {
      ours = static_cast<std::int64_t>(
          sovereign::adapters::GetInteger<T>(nlohmann::json::parse(R"({"v":)" + text + "}").at("v")));
    } catch (const std::exception&) {
      weReject = true;
    }
    if (goRejects != weReject || (!goRejects && ours != c.at("value").get<std::int64_t>())) {
      ++failures;
      std::cerr << typeName << " " << text << ": Go " << (goRejects ? "rejects" : c.at("value").dump()) << ", we "
                << (weReject ? "reject" : std::to_string(ours)) << "\n";
    }
  }
  return failures;
}

bool Accepts(const std::string& port) {
  try {
    (void)nlohmann::json::parse(R"({"type":"anytls","tag":"p","server":"example.com","server_port":)" + port + "}")
        .get<AnyTLSOutbound>();
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <integers.golden.json>\n";
    return 2;
  }
  try {
    const nlohmann::json golden = LoadFixture(argv[1]);
    int failures = Check<std::uint16_t>(golden.at("uint16"), "uint16") +
                   Check<std::int64_t>(golden.at("int64"), "int64");

    for (const char* port : {"443", "0"}) {
      if (!Accepts(port)) {
        ++failures;
        std::cerr << "generated AnyTLS outbound rejects server_port " << port << "\n";
      }
    }
    // No server_port at all: Go leaves it zero, it doesn't reject the config.
    try {
      const auto outbound = nlohmann::json::parse(R"({"type":"anytls","server":"example.com"})").get<AnyTLSOutbound>();
      if (outbound.options.serverPort != 0) {
        ++failures;
        std::cerr << "a missing server_port isn't zero\n";
      }
    } catch (const std::exception& e) {
      ++failures;
      std::cerr << "generated AnyTLS outbound rejects a config without server_port: " << e.what() << "\n";
    }
    for (const char* port : {"70000", "-1", "1.5", "\"443\""}) {
      if (Accepts(port)) {
        ++failures;
        std::cerr << "generated AnyTLS outbound accepts server_port " << port << " (Go rejects it)\n";
      }
    }

    if (failures != 0) {
      std::cerr << failures << " integer cases differ from Go\n";
      return 1;
    }
    std::cout << "OK: integer fields follow Go\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
