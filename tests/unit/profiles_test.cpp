// The configurations' list (src/tray/profiles.h): ids that are safe folder
// names, names, Profile-Title, which one is in use after a removal, and
// tray.json's "profiles" read back - and read defensively.

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "profiles.h"
#include "share_links.h"

namespace {

using namespace sovereign::tray;

Profile Make(std::string id, std::string name = {}, std::string url = {}) {
  Profile p;
  p.id = std::move(id);
  p.name = std::move(name);
  p.url = std::move(url);
  return p;
}

std::vector<Profile> Three() {
  return {Make("p1", "A", "https://a.example.com/s"), Make("p2", "B"), Make("p3", "C", "https://c.example.com/s")};
}

void TestIds() {
  CHECK(IsProfileId("p1"));
  CHECK(IsProfileId("p1790770050-2"));
  CHECK(!IsProfileId(""));
  CHECK(!IsProfileId(".."));
  CHECK(!IsProfileId("../x"));
  CHECK(!IsProfileId("a\\b"));
  CHECK(!IsProfileId("P1"));   // lowercase only: one folder, one id
  CHECK(!IsProfileId("con"));  // Windows' device names
  CHECK(!IsProfileId("com1"));
  CHECK(!IsProfileId("lpt9"));
  CHECK(IsProfileId("com12"));
  CHECK(!IsProfileId(std::string(kMaxProfileId + 1, 'a')));

  const auto profiles = Three();
  CHECK(NewProfileId(profiles, 7) == "p7");
  CHECK(NewProfileId({Make("p7")}, 7) == "p7-2");
  CHECK(NewProfileId({Make("p7"), Make("p7-2")}, 7) == "p7-3");
  CHECK(IsProfileId(NewProfileId(profiles, 1790770050)));
}

void TestNames() {
  const auto profiles = Three();
  CHECK(UniqueProfileName(profiles, "D") == "D");
  CHECK(UniqueProfileName(profiles, "A") == "A 2");
  CHECK(CleanProfileName("  NL\x01 1\t") == "NL 1");
  CHECK(CleanProfileName(std::string(100, 'x')).size() == kMaxProfileName);
  // Cut by characters, on a UTF-8 boundary.
  std::string cyrillic;
  for (int i = 0; i < 70; ++i) {
    cyrillic += "Я";
  }
  CHECK(CleanProfileName(cyrillic).size() == kMaxProfileName * 2);
  CHECK(DefaultProfileName("https://user@sub.example.com:8443/api?token=x") == "sub.example.com:8443");
  CHECK(DefaultProfileName("") == "Мой конфиг");

  CHECK(ParseProfileTitle("My VPN") == std::optional<std::string>("My VPN"));
  CHECK(ParseProfileTitle("base64:" + EncodeBase64("Мой VPN 🇳🇱")) == std::optional<std::string>("Мой VPN 🇳🇱"));
  CHECK(ParseProfileTitle("BASE64:" + EncodeBase64("x")) == std::optional<std::string>("x"));
  CHECK(!ParseProfileTitle("base64:!!!"));
  CHECK(!ParseProfileTitle("   "));
}

void TestFind() {
  const auto profiles = Three();
  CHECK(FindProfile(profiles, "p2") != nullptr && FindProfile(profiles, "p2")->name == "B");
  CHECK(FindProfile(profiles, "p9") == nullptr);
  CHECK(FindProfileByUrl(profiles, "https://c.example.com/s") == &profiles[2]);
  CHECK(FindProfileByUrl(profiles, "") == nullptr);  // the user's own configs have no link to match
}

void TestServers() {
  Profile p = Make("p1");
  CHECK(IsServerEnabled(p, "NL"));
  SetServerEnabled(p, "NL", false);
  SetServerEnabled(p, "NL", false);  // once
  CHECK(!IsServerEnabled(p, "NL") && p.disabled.size() == 1);
  SetServerEnabled(p, "NL", true);
  CHECK(IsServerEnabled(p, "NL") && p.disabled.empty());
  p.updateHours = 12;
  CHECK(RefreshHours(p) == 12);
  p.userHours = 3;
  CHECK(RefreshHours(p) == 3);
}

void TestJson() {
  auto profiles = Three();
  profiles[0].lastRefresh = 1790770050;
  profiles[0].updateHours = 6;
  profiles[0].autoUpdate = false;
  profiles[0].userHours = 48;
  profiles[0].disabled = {"NL", "FI"};
  profiles[1].enabled = false;
  const auto back = ProfilesFromJson(nlohmann::json::parse(ProfilesToJson(profiles).dump()));
  CHECK(back.size() == 3);
  if (back.size() == 3) {
    CHECK(back[0].id == "p1" && back[0].url == "https://a.example.com/s" && back[0].lastRefresh == 1790770050);
    CHECK(back[0].updateHours == 6 && !back[0].autoUpdate && back[0].userHours == 48);
    CHECK(back[0].disabled == std::vector<std::string>({"NL", "FI"}));
    CHECK(back[0].enabled && !back[1].enabled && back[2].enabled);
  }

  // tray.json is a file: what's wrong in it is dropped, the rest kept.
  const auto read = ProfilesFromJson(nlohmann::json::parse(R"([
    {"id":"p1","name":"ok","url":""},
    {"id":"..\\..\\Windows","name":"escape"},
    {"id":"p1","name":"twice"},
    {"id":"p2","name":5},
    "not an object",
    {"id":"p3","name":"","updateHours":100000,"userHours":-5,"disabled":["a",1,"a"]},
    {"id":"p4","name":"x","lastRefresh":"soon","enabled":"yes"}
  ])"));
  CHECK(read.size() == 3);
  if (read.size() == 3) {
    CHECK(read[0].name == "ok" && read[1].id == "p3" && read[2].id == "p4");
    CHECK(!read[1].name.empty() && read[1].updateHours == 24 * 7 && read[1].userHours == 0);
    CHECK(read[1].disabled == std::vector<std::string>({"a"}));
    CHECK(read[2].lastRefresh == 0 && read[2].enabled);
  }
  CHECK(ProfilesFromJson(nlohmann::json::parse(R"({"p1":{}})")).empty());

  // 0.4.3's list: one in use, the others weren't running.
  const auto legacy = ProfilesFromJson(
      nlohmann::json::parse(R"([{"id":"p1","name":"A","url":""},{"id":"p2","name":"B","url":""}])"), "p2");
  CHECK(legacy.size() == 2 && !legacy[0].enabled && legacy[1].enabled);
}
}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestIds();
    TestNames();
    TestFind();
    TestServers();
    TestJson();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
