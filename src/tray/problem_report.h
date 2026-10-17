#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// "Сообщить о проблеме" (the tools' menu): a report of the client for
// a GitHub issue - public, so stricter than the provider's (support_report.h):
// nothing of where the user goes or what they use. The version, Windows, the
// state and the settings that change behaviour, the protocols of the servers
// (no names, addresses, keys) and the core's last warnings and errors with
// every address, name, link, server tag and key in them cut. The pure part,
// unit-tested in tests/unit/problem_report_test.cpp.

namespace sovereign::tray {

struct ProblemInput {
  std::string version;  // "0.5.18"
  std::string windows;  // "Windows 11 24H2 (26100)"
  std::string state;    // the main screen's word: "подключено", "ошибка: …"
  bool selective = false;   // only chosen things through the VPN
  bool killSwitch = false;
  bool lanClosed = false;
  bool warp = false;
  std::vector<std::string> protocols;  // the servers' ("vless · REALITY"), as many as there are
  std::vector<std::string> logLines;   // the core's warnings and errors, newest last, as the log shows them
};

// `text` with everything that tells where or through what the user goes cut:
// links, IPv4 and IPv6 addresses (with ports), domain names, UUIDs, long
// tokens and the server tags a log line names ("outbound/vless[🇩🇪 Mine]").
std::string Anonymize(std::string_view text);

// The report, as the issue's "Отчёт клиента" field takes it.
std::string BuildProblemReport(const ProblemInput& input);

// The address of a new issue on the repository with the bug form filled: the
// version and the report (cut to `maxReport` bytes on a line's end, a note
// that the rest is on the clipboard), percent-encoded.
std::string IssueUrl(std::string_view version, std::string_view report, std::size_t maxReport);

}  // namespace sovereign::tray
