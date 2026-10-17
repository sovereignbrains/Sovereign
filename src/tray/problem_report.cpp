#include "problem_report.h"

#include <algorithm>
#include <format>
#include <functional>
#include <regex>
#include <utility>

namespace sovereign::tray {

namespace {

// Every match of `pattern` in `text` replaced by what `with` makes of it.
std::string ReplaceEach(const std::string& text, const std::regex& pattern,
                        const std::function<std::string(const std::smatch&)>& with) {
  std::string out;
  auto from = text.cbegin();
  for (std::sregex_iterator it(text.cbegin(), text.cend(), pattern), end; it != end; ++it) {
    out.append(from, (*it)[0].first);
    out += with(*it);
    from = (*it)[0].second;
  }
  out.append(from, text.cend());
  return out;
}

std::string Replace(const std::string& text, const std::regex& pattern, const std::string& with) {
  return ReplaceEach(text, pattern, [&](const std::smatch&) { return with; });
}

bool IsHexLetter(char c) { return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::string Percent(std::string_view text) {
  std::string out;
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' ||
        byte == '_' || byte == '.' || byte == '~') {
      out += c;
    } else {
      out += std::format("%{:02X}", byte);
    }
  }
  return out;
}

}  // namespace

std::string Anonymize(std::string_view text) {
  static const std::regex kLink(R"([A-Za-z][A-Za-z0-9+.-]*://[^\s"'<>]+)");
  static const std::regex kUuid(R"([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})");
  // A server's tag in the core's words: "outbound/vless[🇩🇪 Mine]" - the name is the user's or the provider's.
  static const std::regex kTag(R"(((?:outbound|inbound|endpoint)/[A-Za-z0-9_-]+)\[[^\]]*\])");
  static const std::regex kIpv6Bracketed(R"(\[[0-9A-Fa-f.:]*:[0-9A-Fa-f.:]*\](?::\d+)?)");
  // Bare: groups of hex with colons - a clock ("13:42:02") is digits alone and no "::", so it's kept.
  static const std::regex kIpv6(R"((?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4})");
  static const std::regex kIpv4(R"(\b(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?\b)");
  static const std::regex kDomain(R"(\b(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,}(?::\d+)?\b)");
  static const std::regex kToken(R"([A-Za-z0-9+/_-]{32,}={0,2})");

  std::string out(text);
  out = Replace(out, kLink, "<ссылка>");
  out = Replace(out, kUuid, "<uuid>");
  out = ReplaceEach(out, kTag, [](const std::smatch& m) { return m[1].str() + "[…]"; });
  out = Replace(out, kIpv6Bracketed, "<ip>");
  out = ReplaceEach(out, kIpv6, [](const std::smatch& m) {
    const std::string s = m[0].str();
    const bool address = s.find("::") != std::string::npos || std::any_of(s.begin(), s.end(), IsHexLetter);
    return address ? std::string("<ip>") : s;
  });
  out = Replace(out, kIpv4, "<ip>");
  out = Replace(out, kDomain, "<домен>");
  out = Replace(out, kToken, "<ключ>");
  return out;
}

std::string BuildProblemReport(const ProblemInput& input) {
  std::string report = std::format("Sovereign {} · {}\n", input.version, input.windows);
  report += std::format("Состояние: {}\n", input.state);
  report += std::format("Маршруты: {} · Kill switch: {} · Локальная сеть: {} · WARP: {}\n",
                        input.selective ? "через VPN только выбранное" : "через VPN всё",
                        input.killSwitch ? "вкл" : "выкл", input.lanClosed ? "закрыта" : "открыта",
                        input.warp ? "вкл" : "выкл");
  // The protocols in their order, each once with how many servers use it.
  std::vector<std::pair<std::string, int>> protocols;
  for (const std::string& protocol : input.protocols) {
    const auto it = std::find_if(protocols.begin(), protocols.end(), [&](const auto& p) { return p.first == protocol; });
    if (it == protocols.end()) {
      protocols.emplace_back(protocol, 1);
    } else {
      ++it->second;
    }
  }
  std::string servers;
  for (const auto& [protocol, count] : protocols) {
    servers += std::format("{}{}{}", servers.empty() ? "" : ", ", protocol, count > 1 ? std::format(" ×{}", count) : "");
  }
  report += std::format("Серверы: {}\n", servers.empty() ? std::string("нет") : servers);
  if (input.logLines.empty()) {
    report += "Предупреждений и ошибок ядра нет.\n";
  } else {
    report += "Последние предупреждения и ошибки ядра:\n";
    for (const std::string& line : input.logLines) {
      report += Anonymize(line) + "\n";
    }
  }
  return report;
}

std::string IssueUrl(std::string_view version, std::string_view report, std::size_t maxReport) {
  std::string body(report);
  if (body.size() > maxReport) {
    // Cut on a line's end; the whole of it waits on the clipboard.
    const std::size_t end = body.rfind('\n', maxReport);
    body = body.substr(0, end == std::string::npos ? maxReport : end + 1) +
           "… (дальше не влезло в ссылку — весь отчёт в буфере обмена, вставь его сюда)\n";
  }
  return std::format("https://github.com/sovereignbrains/Sovereign/issues/new?template=bug_report.yml&version={}&report={}",
                     Percent(version), Percent(body));
}

}  // namespace sovereign::tray
