// Golden test: badoption.Duration's formatting and parsing (adapters/duration.h)
// against the pinned sing code itself - tests/golden/fixtures/durations.golden.json,
// regenerate with `go run ./tools/goldengen -fixture durations`. Every
// "format" case must print exactly as time.Duration.String() does, every
// "parse" case must give the same nanoseconds, or fail where Go fails (null).
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "adapters/duration.h"

namespace {

nlohmann::json LoadFixture(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open fixture: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return nlohmann::json::parse(buffer.str());
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <durations.golden.json>\n";
    return 2;
  }
  try {
    const nlohmann::json golden = LoadFixture(argv[1]);
    int failures = 0;
    int cases = 0;

    for (const auto& c : golden.at("format")) {
      ++cases;
      const auto ns = c.at("ns").get<std::int64_t>();
      const auto expected = c.at("text").get<std::string>();
      const std::string actual = sovereign::adapters::FormatDuration(std::chrono::nanoseconds{ns});
      if (actual != expected) {
        ++failures;
        std::cerr << "format " << ns << ": expected \"" << expected << "\", got \"" << actual << "\"\n";
      }
    }

    for (const auto& c : golden.at("parse")) {
      ++cases;
      const auto text = c.at("text").get<std::string>();
      const bool shouldFail = c.at("ns").is_null();
      try {
        const auto actual = sovereign::adapters::ParseDuration(text).count();
        if (shouldFail) {
          ++failures;
          std::cerr << "parse \"" << text << "\": Go rejects it, we gave " << actual << "\n";
        } else if (actual != c.at("ns").get<std::int64_t>()) {
          ++failures;
          std::cerr << "parse \"" << text << "\": expected " << c.at("ns").get<std::int64_t>() << ", got " << actual << "\n";
        }
      } catch (const std::invalid_argument& e) {
        if (!shouldFail) {
          ++failures;
          std::cerr << "parse \"" << text << "\": Go accepts it, we threw: " << e.what() << "\n";
        }
      }
    }

    if (failures != 0) {
      std::cerr << failures << " of " << cases << " duration cases differ from Go\n";
      return 1;
    }
    std::cout << "OK: " << cases << " duration cases match Go\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
