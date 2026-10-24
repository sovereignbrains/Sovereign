#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The updater's pure rules, apart from WinHTTP and the installer launch
// (updater.h) so they can be unit-tested (tests/unit/update_test.cpp): which
// release is newer, which of its files is the installer, what its checksum is.

namespace sovereign::tray {

// Where releases come from: this repository's GitHub releases.
inline constexpr std::string_view kReleasesApi = "https://api.github.com/repos/sovereignbrains/Sovereign/releases/latest";
inline constexpr std::string_view kReleaseDownloads = "https://github.com/sovereignbrains/Sovereign/releases/download/";

struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;
  auto operator<=>(const Version&) const = default;
};

// 0.0.0: a build for development (CMakeLists.txt) - no updates for it.
inline bool IsDevelopmentBuild(const Version& v) { return v == Version{}; }

// What the updater does unasked (tray.json "updates", the settings' row):
// Check looks a minute after start and every 12 hours and says what it
// found; Install also installs it (Windows asks first: UAC); Off looks only
// when the user asks.
enum class UpdateMode : std::uint8_t { Off, Check, Install };
std::string_view UpdateModeName(UpdateMode mode);  // "off" / "check" / "install"
UpdateMode ParseUpdateMode(std::string_view name);  // unknown -> Check

// "1.2.3" or "v1.2.3"; nullopt for anything else (pre-release suffixes
// included - the updater only follows plain releases).
std::optional<Version> ParseVersion(std::string_view text);
std::string ToString(const Version& version);

// The installer a release carries: Sovereign-Setup-1.2.3.exe, and next to it
// its checksum, Sovereign-Setup-1.2.3.exe.sha256.
std::string InstallerName(const Version& version);

struct Release {
  Version version;
  std::string installerUrl;
  std::string checksumUrl;
  std::uint64_t installerSize = 0;
  std::string pageUrl;  // the release's page, for "what's new"
};

// GitHub's /releases/latest answer: the release, if it is a published one
// with a version tag and carries both files, each downloadable from this
// repository's releases (IsReleaseAssetUrl). nullopt otherwise.
std::optional<Release> ParseLatestRelease(std::string_view json);

bool IsReleaseAssetUrl(std::string_view url);

// A sha256sum-style checksum file: "<64 hex>  <name>" lines, or a bare hash.
// The lowercase hash for `fileName`; nullopt if there is none.
std::optional<std::string> ParseChecksum(std::string_view text, std::string_view fileName);

}  // namespace sovereign::tray
