// The report for a GitHub issue (src/tray/problem_report.h): public, so
// nothing of where the user goes or through what may stay in it.

#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "problem_report.h"

namespace {

using namespace sovereign::tray;

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void TestAnonymize() {
  // The core's line, every way it names a place cut; its clock and its words kept.
  const std::string line =
      "13:42:38  ERROR  [2642219761 3ms] outbound/vless[🇩🇪 Мой сервер]: dial tcp 5.83.147.210:443: i/o timeout";
  const std::string cut = Anonymize(line);
  CHECK(cut == "13:42:38  ERROR  [2642219761 3ms] outbound/vless[…]: dial tcp <ip>: i/o timeout");

  CHECK(Anonymize("connection to [2606:4700::6810:84e5]:443 refused") == "connection to <ip> refused");
  CHECK(Anonymize("lookup 2a0c:4ac1:4:3e::a failed") == "lookup <ip> failed");
  CHECK(Anonymize("dns: exchange chatgpt.com: timeout") == "dns: exchange <домен>: timeout");
  CHECK(Anonymize("fetch https://sub.example.com/s?token=abc failed") == "fetch <ссылка> failed");
  CHECK(Anonymize("uuid 6a4e1c3b-5d2f-4b8e-9c7a-1f0e3d2b4a61 rejected") == "uuid <uuid> rejected");
  CHECK(Anonymize("key puc15Qa0uCjcB6PH3vPVU37xvBvlvo3o8IPcG/6Ete8= bad") == "key <ключ> bad");
  // Not addresses: a clock, a version, words.
  CHECK(Anonymize("13:42:02 sing-box 1.14.2 started") == "13:42:02 sing-box 1.14.2 started");
}

void TestReport() {
  ProblemInput input;
  input.version = "0.5.18";
  input.windows = "Windows 11 24H2 (26100)";
  input.state = "подключено";
  input.selective = true;
  input.killSwitch = true;
  input.lanClosed = true;
  input.protocols = {"vless · REALITY", "anytls · REALITY", "vless · REALITY"};
  input.logLines = {"13:42:38  WARN  outbound/anytls[AnyTLS · aeza]: dial 5.83.147.210:443: timeout"};
  const std::string report = BuildProblemReport(input);
  CHECK(report.starts_with("Sovereign 0.5.18 · Windows 11 24H2 (26100)\n"));
  CHECK(Has(report, "Маршруты: через VPN только выбранное · Kill switch: вкл · Локальная сеть: закрыта · WARP: выкл"));
  CHECK(Has(report, "Серверы: vless · REALITY ×2, anytls · REALITY"));
  CHECK(Has(report, "outbound/anytls[…]: dial <ip>: timeout"));
  CHECK(!Has(report, "aeza") && !Has(report, "5.83.147.210"));
  input.logLines.clear();
  input.protocols.clear();
  const std::string quiet = BuildProblemReport(input);
  CHECK(Has(quiet, "Серверы: нет") && Has(quiet, "Предупреждений и ошибок ядра нет."));
}

void TestUrl() {
  const std::string url = IssueUrl("0.5.18", "строка 1\nстрока 2\n", 1000);
  CHECK(url.starts_with("https://github.com/sovereignbrains/Sovereign/issues/new?template=bug_report.yml&version=0.5.18&report="));
  CHECK(Has(url, "%0A") && !Has(url, " ") && !Has(url, "\n"));
  // Too long for a link: cut on a line, the rest said to be on the clipboard.
  const std::string cut = IssueUrl("0.5.18", "aaaa\nbbbb\ncccc\n", 7);
  CHECK(Has(cut, "report=aaaa%0A%E2%80%A6") && !Has(cut, "bbbb"));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestAnonymize();
    TestReport();
    TestUrl();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
