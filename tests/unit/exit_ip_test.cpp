// The service's box_exitip answer (src/tray/exit_ip.h).

#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "exit_ip.h"

namespace {

using sovereign::tray::ParseExitIpResponse;

void TestParses() {
  const auto v4 = ParseExitIpResponse(R"({"cmd":"box_exitip","tag":"nl","ip":"185.12.34.56","country":"NL"})");
  CHECK(v4 && v4->ip == "185.12.34.56" && v4->country == "NL" && !v4->pending && v4->error.empty());
  const auto v6 = ParseExitIpResponse(R"({"cmd":"box_exitip","tag":"nl","ip":"2a01:4f8::1"})");
  CHECK(v6 && v6->ip == "2a01:4f8::1" && v6->country.empty());
  const auto pending = ParseExitIpResponse(R"({"cmd":"box_exitip","tag":"nl","pending":true})");
  CHECK(pending && pending->pending && pending->ip.empty());
  const auto failed = ParseExitIpResponse(R"({"cmd":"box_exitip","tag":"nl","error":"i/o timeout"})");
  CHECK(failed && failed->error == "i/o timeout");
}

void TestDropsWhatDoesNotFit() {
  const auto odd = ParseExitIpResponse(
      R"({"cmd":"box_exitip","ip":"<script>","country":"nl","pending":"yes","error":5})");
  CHECK(odd && odd->ip.empty() && odd->country.empty() && !odd->pending && odd->error.empty());
  const auto longIp = ParseExitIpResponse(R"({"cmd":"box_exitip","ip":")" + std::string(60, '1') + R"("})");
  CHECK(longIp && longIp->ip.empty());
  const auto longCountry = ParseExitIpResponse(R"({"cmd":"box_exitip","country":"NLD"})");
  CHECK(longCountry && longCountry->country.empty());
  const auto longError = ParseExitIpResponse(R"({"cmd":"box_exitip","error":")" + std::string(1000, 'e') + R"("})");
  CHECK(longError && longError->error.size() == 300);
}

void TestIsp() {
  const auto full = ParseExitIpResponse(
      R"({"cmd":"box_exitip","tag":"nl","ip":"185.12.34.56","country":"NL","isp":"Hetzner Online GmbH"})");
  CHECK(full && full->isp == "Hetzner Online GmbH");
  const auto twoLines = ParseExitIpResponse(R"({"cmd":"box_exitip","isp":"a\nb"})");
  CHECK(twoLines && twoLines->isp.empty());
  const auto number = ParseExitIpResponse(R"({"cmd":"box_exitip","isp":5})");
  CHECK(number && number->isp.empty());
  const auto tooLong = ParseExitIpResponse(R"({"cmd":"box_exitip","isp":")" + std::string(300, 'x') + R"("})");
  CHECK(tooLong && tooLong->isp.empty());

  using sovereign::tray::ShortIsp;
  CHECK(ShortIsp("Hetzner Online GmbH") == "Hetzner Online");
  CHECK(ShortIsp("AEZA INTERNATIONAL LTD") == "AEZA INTERNATIONAL");
  CHECK(ShortIsp("Brainoza OU") == "Brainoza");
  CHECK(ShortIsp("DigitalOcean, LLC") == "DigitalOcean");
  CHECK(ShortIsp("Example, Inc.") == "Example");
  CHECK(ShortIsp("OVH SAS") == "OVH");
  CHECK(ShortIsp("OOO Selectel") == "Selectel");
  CHECK(ShortIsp("Stark Industries Solutions Ltd") == "Stark Industries Solutions");
  CHECK(ShortIsp("Cloudflare") == "Cloudflare");
  CHECK(ShortIsp("Philip Fjaera trading as PFWeb Solutions") == "PFWeb Solutions");
  CHECK(ShortIsp("AEZA GROUP LLC") == "AEZA GROUP");
  CHECK(ShortIsp("LLC") == "LLC");  // nothing else: the whole name
  CHECK(ShortIsp("Ltd. Co.") == "Ltd.");
  CHECK(ShortIsp("") == "");
}

void TestRejectsOtherAnswers() {
  CHECK(!ParseExitIpResponse(R"({"cmd":"error","message":"gocore not loaded"})"));
  CHECK(!ParseExitIpResponse("not json"));
  CHECK(!ParseExitIpResponse("[1]"));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestParses();
    TestDropsWhatDoesNotFit();
    TestIsp();
    TestRejectsOtherAnswers();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
