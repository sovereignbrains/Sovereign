// The config and its subscription (src/tray/config_sync.h).

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "config_sync.h"

namespace {

using nlohmann::json;
using sovereign::tray::Arrival;
using sovereign::tray::ClassifyArrival;
using sovereign::tray::MergeConfigs;
using sovereign::tray::SameConfig;

bool Has(const std::vector<std::string>& list, const std::string& item) {
  return std::find(list.begin(), list.end(), item) != list.end();
}

void TestSameConfig() {
  CHECK(SameConfig(R"({"a":1,"b":[1,2]})", "{\n  \"b\": [1, 2],\n  \"a\": 1.0\n}"));
  CHECK(!SameConfig(R"({"a":1})", R"({"a":2})"));
  CHECK(!SameConfig(R"({"b":[1,2]})", R"({"b":[2,1]})"));  // arrays keep their order
  CHECK(SameConfig("not json", "not json"));
  CHECK(!SameConfig("not json", R"({})"));
}

void TestClassify() {
  const std::string a = R"({"outbounds":[{"type":"direct","tag":"a"}]})";
  const std::string b = R"({"outbounds":[{"type":"direct","tag":"b"}]})";
  const std::string edited = R"({"outbounds":[{"type":"direct","tag":"mine"}]})";
  CHECK(ClassifyArrival(std::nullopt, std::nullopt, a) == Arrival::Replace);  // the first one
  CHECK(ClassifyArrival(a, a, a) == Arrival::Unchanged);
  CHECK(ClassifyArrival(a, a, b) == Arrival::Replace);             // no edits: silently
  CHECK(ClassifyArrival(a, edited, b) == Arrival::Ask);            // edits: the user chooses
  CHECK(ClassifyArrival(a, edited, a) == Arrival::Unchanged);      // the same subscription again
  CHECK(ClassifyArrival(a, b, b) == Arrival::Replace);             // edited into what came
  // From before subscription.json: config.json is the subscription as it was.
  CHECK(ClassifyArrival(std::nullopt, a, a) == Arrival::Unchanged);
  CHECK(ClassifyArrival(std::nullopt, a, b) == Arrival::Replace);
  // Reformatting isn't an edit.
  CHECK(ClassifyArrival(a, json::parse(a).dump(4), b) == Arrival::Replace);
}

// The user changed one server's port; the subscription changed another's
// address and added a server: both land, nothing clashes.
void TestIndependentEditsBothLand() {
  const std::string base = R"({"outbounds":[
    {"type":"vless","tag":"nl","server":"nl.example","server_port":443},
    {"type":"vless","tag":"fi","server":"fi.example","server_port":443}]})";
  const std::string mine = R"({"outbounds":[
    {"type":"vless","tag":"nl","server":"nl.example","server_port":8443},
    {"type":"vless","tag":"fi","server":"fi.example","server_port":443}]})";
  const std::string theirs = R"({"outbounds":[
    {"type":"vless","tag":"nl","server":"nl.example","server_port":443},
    {"type":"vless","tag":"fi","server":"fi2.example","server_port":443},
    {"type":"vless","tag":"de","server":"de.example","server_port":443}]})";
  const auto merged = MergeConfigs(base, mine, theirs);
  CHECK(merged.has_value());
  if (!merged) {
    return;
  }
  const json c = json::parse(merged->config);
  CHECK(c["outbounds"].size() == 3);
  CHECK(c["outbounds"][0]["server_port"] == 8443);
  CHECK(c["outbounds"][1]["server"] == "fi2.example");
  CHECK(c["outbounds"][2]["tag"] == "de");
  CHECK(merged->conflicts.empty());
}

void TestBothChangedTheSameValue() {
  const auto merged = MergeConfigs(R"({"log":{"level":"warn"}})", R"({"log":{"level":"debug"}})",
                                   R"({"log":{"level":"error"}})");
  CHECK(merged.has_value());
  if (!merged) {
    return;
  }
  CHECK(json::parse(merged->config)["log"]["level"] == "debug");  // the user's side wins
  CHECK(Has(merged->conflicts, "log.level"));
}

void TestTaggedPathsNameTheElement() {
  const auto merged = MergeConfigs(R"({"outbounds":[{"tag":"nl","server":"a"}]})",
                                   R"({"outbounds":[{"tag":"nl","server":"mine"}]})",
                                   R"({"outbounds":[{"tag":"nl","server":"theirs"}]})");
  CHECK(merged && Has(merged->conflicts, "outbounds[nl].server"));
}

// A rule the user added stays where they put it - after the rule it followed
// - in the new version's rules; one they deleted stays deleted.
void TestRulesKeepTheUsersAdditionsAndRemovals() {
  const std::string base = R"({"route":{"rules":[
    {"action":"sniff"},
    {"protocol":"dns","action":"hijack-dns"},
    {"rule_set":"ads","action":"reject"},
    {"ip_is_private":true,"outbound":"direct"}]}})";
  const std::string mine = R"({"route":{"rules":[
    {"action":"sniff"},
    {"protocol":"dns","action":"hijack-dns"},
    {"domain_suffix":["example.org"],"outbound":"direct"},
    {"ip_is_private":true,"outbound":"direct"}]}})";
  const std::string theirs = R"({"route":{"rules":[
    {"action":"sniff"},
    {"protocol":"dns","action":"hijack-dns"},
    {"rule_set":"ads","action":"reject"},
    {"rule_set":"ru","outbound":"direct"},
    {"ip_is_private":true,"outbound":"direct"}]}})";
  const auto merged = MergeConfigs(base, mine, theirs);
  CHECK(merged.has_value());
  if (!merged) {
    return;
  }
  const json rules = json::parse(merged->config)["route"]["rules"];
  CHECK(rules.size() == 5);
  CHECK(rules[0]["action"] == "sniff");
  CHECK(rules[1]["action"] == "hijack-dns");
  CHECK(rules[2]["domain_suffix"][0] == "example.org");  // after hijack-dns, as in the user's version
  CHECK(rules[3]["rule_set"] == "ru");                    // the subscription's new rule
  // The ads rule: deleted by the user, unchanged by the subscription.
  CHECK(std::none_of(rules.begin(), rules.end(), [](const json& r) { return r.value("rule_set", "") == "ads"; }));
  CHECK(Has(merged->conflicts, "route.rules"));  // both touched the list: worth a look
}

void TestUsersNewKeysAndDeletions() {
  const auto merged = MergeConfigs(R"({"dns":{"strategy":"ipv4_only"},"log":{"level":"warn"}})",
                                   R"({"dns":{"strategy":"ipv4_only"},"experimental":{"clash_api":{}}})",
                                   R"({"dns":{"strategy":"prefer_ipv4"},"log":{"level":"warn"}})");
  CHECK(merged.has_value());
  if (!merged) {
    return;
  }
  const json c = json::parse(merged->config);
  CHECK(c["dns"]["strategy"] == "prefer_ipv4");
  CHECK(!c.contains("log"));  // the user removed it; the subscription didn't touch it
  CHECK(c.contains("experimental"));
  CHECK(merged->conflicts.empty());
}

// A server the user edited but the subscription dropped: kept, and flagged.
void TestEditedButDropped() {
  const auto merged = MergeConfigs(R"({"outbounds":[{"tag":"a","x":1},{"tag":"b","x":1}]})",
                                   R"({"outbounds":[{"tag":"a","x":2},{"tag":"b","x":1}]})",
                                   R"({"outbounds":[{"tag":"b","x":1}]})");
  CHECK(merged.has_value());
  if (!merged) {
    return;
  }
  const json c = json::parse(merged->config);
  CHECK(c["outbounds"].size() == 2);
  CHECK(c["outbounds"][0]["tag"] == "a");
  CHECK(Has(merged->conflicts, "outbounds[a]"));
}

void TestKeepsTheSubscriptionsKeyOrder() {
  const auto merged = MergeConfigs(R"({"outbounds":[{"type":"direct","tag":"d"}]})",
                                   R"({"outbounds":[{"type":"direct","tag":"d"}]})",
                                   R"({"outbounds":[{"type":"direct","tag":"d","zzz":1}]})");
  CHECK(merged && merged->config.find("\"type\"") < merged->config.find("\"tag\""));
}

void TestNotObjects() {
  CHECK(!MergeConfigs("[]", "{}", "{}"));
  CHECK(!MergeConfigs("{}", "not json", "{}"));
  CHECK(!MergeConfigs("{}", "{}", ""));
}

// Nesting a subscription could use to blow the stack: never recursed into.
void TestDeepNestingIsNotRecursedInto() {
  std::string deep;
  for (int i = 0; i < 1000; ++i) {
    deep += R"({"a":)";
  }
  std::string mine = deep + "1";
  std::string theirs = deep + "2";
  std::string base = deep + "0";
  for (int i = 0; i < 1000; ++i) {
    mine += "}";
    theirs += "}";
    base += "}";
  }
  CHECK(sovereign::tray::NestingDepth(mine) == 1000);
  CHECK(!MergeConfigs(base, mine, theirs));
  CHECK(!SameConfig(base, mine));  // compared as text
  CHECK(SameConfig(mine, mine));
  CHECK(ClassifyArrival(base, mine, theirs) == Arrival::Ask);
  // Brackets in strings don't count. (Not a raw string inside CHECK: MSVC 14.44's
  // preprocessor misreads one with ']]' among a macro's arguments.)
  constexpr std::string_view kInString = R"({"a":"[[[{{{\"]]"})";
  CHECK(sovereign::tray::NestingDepth(kInString) == 1);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestSameConfig();
    TestClassify();
    TestIndependentEditsBothLand();
    TestBothChangedTheSameValue();
    TestTaggedPathsNameTheElement();
    TestRulesKeepTheUsersAdditionsAndRemovals();
    TestUsersNewKeysAndDeletions();
    TestEditedButDropped();
    TestKeepsTheSubscriptionsKeyOrder();
    TestNotObjects();
    TestDeepNestingIsNotRecursedInto();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
