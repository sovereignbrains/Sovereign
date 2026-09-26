// Picking the proxy protocol (src/tray/protocol_choice.h).

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "protocol_choice.h"

namespace {

using nlohmann::json;
using namespace sovereign::tray;

// packetlab's shape: a selector "proxy" in front of a urltest "auto".
constexpr std::string_view kSubscription = R"({
  "outbounds": [
    {"type":"selector","tag":"proxy","outbounds":["auto","AnyTLS-REALITY","TUIC"],"default":"auto"},
    {"type":"urltest","tag":"auto","outbounds":["AnyTLS-REALITY","TUIC"]},
    {"type":"anytls","tag":"AnyTLS-REALITY"},{"type":"tuic","tag":"TUIC"},{"type":"direct","tag":"direct"}],
  "route": {"final":"proxy"}
})";

void TestFindsTheSelector() {
  const ProtocolChoices c = FindProtocolChoices(kSubscription);
  CHECK(c.selector == "proxy");
  CHECK(c.options == std::vector<std::string>({"auto", "AnyTLS-REALITY", "TUIC"}));
  CHECK(c.configDefault == "auto");
}

void TestPickSetsDefault() {
  const json c = json::parse(ApplyProtocolChoice(kSubscription, "TUIC"));
  CHECK(c["outbounds"][0]["default"] == "TUIC");
  CHECK(c["outbounds"][1]["type"] == "urltest");  // nothing else touched
}

void TestUnknownOrEmptyPickChangesNothing() {
  CHECK(json::parse(ApplyProtocolChoice(kSubscription, "")) == json::parse(kSubscription));
  // The subscription dropped the protocol the user had picked.
  CHECK(json::parse(ApplyProtocolChoice(kSubscription, "Hysteria2")) == json::parse(kSubscription));
}

void TestSelectorByFinalAndFallbacks() {
  // route.final names the second selector; without final the first one counts.
  constexpr std::string_view two =
      R"({"outbounds":[{"type":"selector","tag":"a","outbounds":["x"]},{"type":"selector","tag":"b","outbounds":["y","z"]}],"route":{"final":"b"}})";
  CHECK(FindProtocolChoices(two).selector == "b");
  CHECK(FindProtocolChoices(two).configDefault == "y");  // no default: the first option
  CHECK(FindProtocolChoices(R"({"outbounds":[{"type":"selector","tag":"a","outbounds":["x"]}]})").selector == "a");
  CHECK(FindProtocolChoices(R"({"outbounds":[{"type":"direct","tag":"d"}]})").selector.empty());
  CHECK(FindProtocolChoices("not json").options.empty());
  CHECK(ApplyProtocolChoice("not json", "x") == "not json");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestFindsTheSelector();
    TestPickSetsDefault();
    TestUnknownOrEmptyPickChangesNothing();
    TestSelectorByFinalAndFallbacks();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
