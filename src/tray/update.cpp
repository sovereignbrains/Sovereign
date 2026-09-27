#include "update.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

#include "json_field.h"

namespace sovereign::tray {

namespace {

bool IsHex(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
  return out;
}

std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

}  // namespace

std::optional<Version> ParseVersion(std::string_view text) {
  if (text.starts_with('v')) {
    text.remove_prefix(1);
  }
  Version version;
  int* const parts[] = {&version.major, &version.minor, &version.patch};
  for (std::size_t i = 0; i < 3; ++i) {
    if (text.empty() || !std::isdigit(static_cast<unsigned char>(text.front())) ||
        (text.size() > 1 && text.front() == '0' && std::isdigit(static_cast<unsigned char>(text[1])))) {
      return std::nullopt;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *parts[i]);
    if (error != std::errc{} || *parts[i] > 99999) {
      return std::nullopt;
    }
    text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    if (i < 2) {
      if (!text.starts_with('.')) {
        return std::nullopt;
      }
      text.remove_prefix(1);
    }
  }
  return text.empty() ? std::optional(version) : std::nullopt;
}

std::string ToString(const Version& version) {
  return std::format("{}.{}.{}", version.major, version.minor, version.patch);
}

std::string InstallerName(const Version& version) { return "Sovereign-Setup-" + ToString(version) + ".exe"; }

bool IsReleaseAssetUrl(std::string_view url) {
  return url.starts_with(kReleaseDownloads) && url.size() > kReleaseDownloads.size() &&
         std::none_of(url.begin(), url.end(), [](char c) { return static_cast<unsigned char>(c) <= ' ' || c == 0x7F; });
}

std::optional<Release> ParseLatestRelease(std::string_view json) {
  const auto root = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (!root.is_object() || Field<bool>(root, "draft", false) || Field<bool>(root, "prerelease", false)) {
    return std::nullopt;
  }
  const auto version = ParseVersion(Field<std::string>(root, "tag_name", {}));
  if (!version) {
    return std::nullopt;
  }
  Release release;
  release.version = *version;
  release.pageUrl = Field<std::string>(root, "html_url", {});
  const std::string installer = InstallerName(*version);
  const std::string checksum = installer + ".sha256";
  const auto assets = root.find("assets");
  if (assets == root.end() || !assets->is_array()) {
    return std::nullopt;
  }
  for (const auto& asset : *assets) {
    const std::string name = Field<std::string>(asset, "name", {});
    const std::string url = Field<std::string>(asset, "browser_download_url", {});
    if (!IsReleaseAssetUrl(url)) {
      continue;
    }
    if (name == installer) {
      release.installerUrl = url;
      release.installerSize = static_cast<std::uint64_t>(std::max<std::int64_t>(0, Field<std::int64_t>(asset, "size", 0)));
    } else if (name == checksum) {
      release.checksumUrl = url;
    }
  }
  if (release.installerUrl.empty() || release.checksumUrl.empty()) {
    return std::nullopt;
  }
  return release;
}

std::optional<std::string> ParseChecksum(std::string_view text, std::string_view fileName) {
  const std::string_view whole = Trim(text);
  if (whole.size() == 64 && IsHex(whole)) {
    return Lower(whole);
  }
  while (!text.empty()) {
    const auto end = text.find('\n');
    const std::string_view line = Trim(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    if (line.size() < 66 || !IsHex(line.substr(0, 64)) || (line[64] != ' ' && line[64] != '\t')) {
      continue;
    }
    std::string_view name = Trim(line.substr(64));
    if (name.starts_with('*')) {  // sha256sum's binary-mode marker
      name.remove_prefix(1);
    }
    if (name == fileName) {
      return Lower(line.substr(0, 64));
    }
  }
  return std::nullopt;
}

}  // namespace sovereign::tray
