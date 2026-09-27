// The updater's pure rules (src/tray/update.h).

#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "update.h"

namespace {

using namespace sovereign::tray;

void TestVersions() {
  CHECK((ParseVersion("1.2.3") == Version{1, 2, 3}));
  CHECK((ParseVersion("v0.10.0") == Version{0, 10, 0}));
  CHECK(!ParseVersion("1.2"));
  CHECK(!ParseVersion("1.2.3-rc1"));  // pre-releases aren't followed
  CHECK(!ParseVersion("1.2.3.4"));
  CHECK(!ParseVersion("01.2.3"));
  CHECK(!ParseVersion("1..3"));
  CHECK(!ParseVersion("v"));
  CHECK(!ParseVersion(""));
  CHECK(!ParseVersion("1.2.999999999999"));
  CHECK((Version{0, 10, 0} > Version{0, 9, 9}));
  CHECK((Version{1, 0, 0} > Version{0, 99, 99}));
  CHECK((!(Version{0, 2, 0} > Version{0, 2, 0})));
  CHECK(ToString({1, 20, 3}) == "1.20.3");
  CHECK(InstallerName({0, 2, 0}) == "Sovereign-Setup-0.2.0.exe");
}

// Where the v0.2.0 files live (a function: a static std::string may throw at startup).
std::string Base() { return "https://github.com/sovereignbrains/Sovereign/releases/download/v0.2.0/"; }

std::string ReleaseJson(const std::string& extra, const std::string& tag = "v0.2.0") {
  return R"({"tag_name":")" + tag + R"(","html_url":"https://github.com/sovereignbrains/Sovereign/releases/tag/v0.2.0",)" +
         extra + R"("assets":[)" + R"({"name":"Sovereign-Setup-0.2.0.exe","size":31457280,"browser_download_url":")" +
         Base() + R"(Sovereign-Setup-0.2.0.exe"},)" +
         R"({"name":"Sovereign-Setup-0.2.0.exe.sha256","size":90,"browser_download_url":")" + Base() +
         R"(Sovereign-Setup-0.2.0.exe.sha256"}]})";
}

void TestLatestRelease() {
  const auto release = ParseLatestRelease(ReleaseJson(""));
  CHECK(release.has_value());
  if (release) {
    CHECK((release->version == Version{0, 2, 0}));
    CHECK(release->installerUrl == Base() + "Sovereign-Setup-0.2.0.exe");
    CHECK(release->checksumUrl == Base() + "Sovereign-Setup-0.2.0.exe.sha256");
    CHECK(release->installerSize == 31457280);
  }
  CHECK(!ParseLatestRelease(ReleaseJson(R"("draft":true,)")));
  CHECK(!ParseLatestRelease(ReleaseJson(R"("prerelease":true,)")));
  CHECK(!ParseLatestRelease(ReleaseJson("", "nightly")));
  // The tag says 0.3.0 but the files are 0.2.0's: no installer for this release.
  CHECK(!ParseLatestRelease(ReleaseJson("", "v0.3.0")));
  // Files from anywhere but this repository's releases are not downloaded.
  CHECK(!IsReleaseAssetUrl("https://evil.example/Sovereign-Setup-0.2.0.exe"));
  CHECK(!IsReleaseAssetUrl("http://github.com/sovereignbrains/Sovereign/releases/download/v0.2.0/x.exe"));
  CHECK(!IsReleaseAssetUrl("https://github.com/someone/Sovereign/releases/download/v0.2.0/x.exe"));
  CHECK(!IsReleaseAssetUrl(Base() + "a b.exe"));
  CHECK(IsReleaseAssetUrl(Base() + "Sovereign-Setup-0.2.0.exe"));
  CHECK(!ParseLatestRelease("not json"));
  CHECK(!ParseLatestRelease(R"({"tag_name":"v0.2.0","assets":{}})"));
  CHECK(!ParseLatestRelease(R"({"message":"API rate limit exceeded"})"));
}

void TestChecksum() {
  const std::string hash = "9F86D081884C7D659A2FEAA0C55AD015A3BF4F1B2B0B822CD15D6C15B0F00A08";
  const std::string lower = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
  CHECK(ParseChecksum(hash + "\n", "x.exe") == lower);
  CHECK(ParseChecksum(hash + "  Sovereign-Setup-0.2.0.exe\r\n", "Sovereign-Setup-0.2.0.exe") == lower);
  CHECK(ParseChecksum(hash + " *Sovereign-Setup-0.2.0.exe\n", "Sovereign-Setup-0.2.0.exe") == lower);
  CHECK(ParseChecksum("other  a.exe\n" + hash + "  b.exe\n", "b.exe") == lower);
  CHECK(!ParseChecksum(hash + "  other.exe\n", "Sovereign-Setup-0.2.0.exe"));
  CHECK(!ParseChecksum("zz" + hash.substr(2) + "  a.exe", "a.exe"));
  CHECK(!ParseChecksum("", "a.exe"));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestVersions();
    TestLatestRelease();
    TestChecksum();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
