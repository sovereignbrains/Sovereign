#include "exit_ip.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include "json_field.h"

namespace sovereign::tray {

namespace {

constexpr std::size_t kMaxAddress = 45;  // the longest IPv6 text form
constexpr std::size_t kMaxError = 300;
constexpr std::size_t kMaxIsp = 256;  // bytes: gocore cuts at 64 characters

bool LooksLikeAddress(const std::string& ip) {
  return !ip.empty() && ip.size() <= kMaxAddress && std::all_of(ip.begin(), ip.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':';
  });
}

bool LooksLikeCountry(const std::string& code) {
  return code.size() == 2 && code[0] >= 'A' && code[0] <= 'Z' && code[1] >= 'A' && code[1] <= 'Z';
}

// One line of text, not too long: it's drawn in the window.
bool LooksLikeIsp(const std::string& isp) {
  return !isp.empty() && isp.size() <= kMaxIsp &&
         std::none_of(isp.begin(), isp.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
}

std::string Upper(std::string_view word) {
  std::string upper(word);
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
  return upper;
}

// A company's legal form, as a word of its name: "GmbH", "LLC", "Ltd.", "OU".
bool IsLegalForm(std::string_view word) {
  while (!word.empty() && (word.back() == '.' || word.back() == ',')) {
    word.remove_suffix(1);
  }
  static constexpr std::array<std::string_view, 40> kForms = {
      "LLC",  "LTD",  "LIMITED", "GMBH", "INC",  "OU",    "O\xC3\x9C", "AB",   "BV",    "B.V",
      "SA",   "S.A",  "SRL",     "S.R.L", "SAS", "SARL",  "OY",        "AG",   "KFT",   "CO",
      "CORP", "PLC",  "LLP",     "PTE",  "PTY",  "SPA",   "S.P.A",     "UAB",  "SIA",   "D.O.O",
      "S.R.O", "KG",  "NV",      "N.V",  "AS",   "A.S",   "OOO",       "\xD0\x9E\xD0\x9E\xD0\x9E", "JSC", "LTDA"};
  const std::string upper = Upper(word);
  return std::find(kForms.begin(), kForms.end(), upper) != kForms.end();
}

}  // namespace

std::string ShortIsp(std::string_view isp) {
  // "Philip Fjaera trading as PFWeb Solutions": the business, not the owner.
  if (const std::size_t at = Upper(isp).find(" TRADING AS "); at != std::string::npos && at + 12 < isp.size()) {
    isp.remove_prefix(at + 12);
  }
  std::vector<std::string_view> words;
  for (std::size_t i = 0; i < isp.size();) {
    const std::size_t end = std::min(isp.find(' ', i), isp.size());
    if (end > i) {
      words.push_back(isp.substr(i, end - i));
    }
    i = end + 1;
  }
  std::size_t first = 0;
  std::size_t last = words.size();
  while (last > first + 1 && IsLegalForm(words[last - 1])) {
    --last;
  }
  while (last > first + 1 && IsLegalForm(words[first])) {
    ++first;
  }
  std::string name;
  for (std::size_t i = first; i < last; ++i) {
    name += (name.empty() ? "" : " ") + std::string(words[i]);
  }
  while (!name.empty() && name.back() == ',') {  // "Example, Inc."
    name.pop_back();
  }
  return name.empty() ? std::string(isp) : name;
}

std::optional<ExitIp> ParseExitIpResponse(std::string_view response) {
  const auto json = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);
  if (Field<std::string>(json, "cmd", {}) != "box_exitip") {
    return std::nullopt;
  }
  ExitIp exit;
  if (std::string ip = Field<std::string>(json, "ip", {}); LooksLikeAddress(ip)) {
    exit.ip = std::move(ip);
  }
  if (std::string country = Field<std::string>(json, "country", {}); LooksLikeCountry(country)) {
    exit.country = std::move(country);
  }
  if (std::string isp = Field<std::string>(json, "isp", {}); LooksLikeIsp(isp)) {
    exit.isp = std::move(isp);
  }
  exit.pending = Field<bool>(json, "pending", false);
  exit.error = Field<std::string>(json, "error", {}).substr(0, kMaxError);
  return exit;
}

}  // namespace sovereign::tray
