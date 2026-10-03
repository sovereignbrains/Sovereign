// The subscription relay from a console, the tray's own code against a live
// worker (tools/relay): by hand, not a ctest.
//
//   relay-probe <subscription url>
//
// The relay's address and key come from SOVEREIGN_RELAY_URL and
// SOVEREIGN_RELAY_KEY - never on the command line, where they'd stay in the
// shell's history. Prints what came: size, a SHA-256, the subscription's
// headers; never the URL or the key.

// windows.h's min/max macros break std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "fetch.h"
#include "sha256.h"

namespace {

std::string Env(const char* name) {
  const DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
  if (size == 0) {
    return {};
  }
  std::string out(size, '\0');
  out.resize(GetEnvironmentVariableA(name, out.data(), size));
  return out;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    const std::string relay = Env("SOVEREIGN_RELAY_URL");
    const std::string key = Env("SOVEREIGN_RELAY_KEY");
    if (argc != 2 || relay.empty() || key.empty()) {
      std::cerr << "usage: relay-probe <subscription url>, with SOVEREIGN_RELAY_URL and SOVEREIGN_RELAY_KEY set\n";
      return 2;
    }
    const std::string url = argv[1];
    const std::wstring wide(url.begin(), url.end());  // an ASCII URL is enough for a probe
    const auto fetched = sovereign::tray::FetchSubscriptionViaRelay(relay, key, wide, L"sing-box 1.14.2",
                                                                    "0123456789abcdef0123456789abcdef");
    if (!fetched) {
      std::cout << "FAIL " << fetched.error() << "\n";
      return 1;
    }
    std::cout << "OK   " << fetched->body.size() << " bytes, sha256 " << sovereign::Sha256Hex(fetched->body) << "\n";
    if (const auto& interval = fetched->updateInterval; interval.has_value()) {
      std::cout << "     Profile-Update-Interval: " << interval.value().count() << " h\n";
    }
    if (const auto& title = fetched->title; title.has_value()) {
      std::cout << "     Profile-Title: " << title.value() << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "exception: " << e.what() << "\n";
    return 1;
  }
}
