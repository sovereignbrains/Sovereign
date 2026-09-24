#pragma once

// A deliberately tiny check helper: the unit tests here are small enough that
// a test framework dependency (vcpkg port, CI cache, ASan interplay) would
// cost more than it gives. Each test binary returns non-zero if any CHECK
// failed, which is all CTest needs.

#include <iostream>
#include <source_location>

namespace sovereign::test {

inline int& Failures() {
  static int failures = 0;
  return failures;
}

inline void Check(bool ok, const char* expression,
                  std::source_location location = std::source_location::current()) {
  if (!ok) {
    ++Failures();
    std::cerr << location.file_name() << ":" << location.line() << ": CHECK failed: " << expression
              << "\n";
  }
}

}  // namespace sovereign::test

#define CHECK(expression) ::sovereign::test::Check(static_cast<bool>(expression), #expression)
