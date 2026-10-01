// The tray's files (src/tray/settings.h), in a temporary %LOCALAPPDATA%:
// a tray from before profiles has its one configuration moved into
// profiles\p1\ - once, with its subscription, pick and history - a config.json
// put into the folder by hand becomes a configuration too, and profiles'
// files stay in their own folders.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

#include "check.h"
#include "settings.h"

namespace {

namespace fs = std::filesystem;
using namespace sovereign::tray;

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void TestMigration(const fs::path& data) {
  Write(data / "tray.json",
        R"({"wantOn":true,"subscriptionUrl":"https://sub.example.com/s?token=x","lastRefresh":1790770050,)"
        R"("updateHours":6,"protocol":"NL","apps":["steam.exe"]})");
  Write(data / "config.json", R"({"outbounds":[{"type":"direct","tag":"mine"}]})");
  Write(data / "subscription.json", R"({"outbounds":[{"type":"direct","tag":"theirs"}]})");
  Write(data / "history" / "20260930-120000-000.json", "{}");

  const TraySettings settings = LoadSettings();
  CHECK(settings.wantOn);
  CHECK(settings.apps.size() == 1);
  CHECK(settings.profiles.size() == 1);
  CHECK(settings.protocol == "NL");  // the pick is one for all now
  if (!settings.profiles.empty()) {
    const Profile& p = settings.profiles.front();
    CHECK(p.id == "p1");
    CHECK(p.url == "https://sub.example.com/s?token=x");
    CHECK(p.name == "sub.example.com");
    CHECK(p.lastRefresh == 1790770050 && p.updateHours == 6 && p.enabled);
  }
  const fs::path dir = data / "profiles" / "p1";
  CHECK(Read(dir / "config.json").find("mine") != std::string::npos);
  CHECK(Read(dir / "subscription.json").find("theirs") != std::string::npos);
  CHECK(fs::exists(dir / "history" / "20260930-120000-000.json"));
  CHECK(!fs::exists(data / "config.json") && !fs::exists(data / "subscription.json"));
  CHECK(LoadConfig(ProfileDir("p1")) == std::optional<std::string>(Read(dir / "config.json")));

  // tray.json knows the profile now: a second start moves nothing and adds nothing.
  const auto saved = nlohmann::json::parse(Read(data / "tray.json"));
  CHECK(saved.contains("profiles") && !saved.contains("subscriptionUrl"));
  CHECK(LoadSettings().profiles.size() == 1);

  // A config.json dropped in by hand: another configuration, off - another is on.
  Write(data / "config.json", R"({"outbounds":[{"type":"direct","tag":"dropped"}]})");
  const TraySettings again = LoadSettings();
  CHECK(again.profiles.size() == 2);
  if (again.profiles.size() == 2) {
    CHECK(again.profiles[0].enabled && !again.profiles[1].enabled);
    CHECK(again.profiles[1].url.empty() && again.profiles[1].name == "Мой конфиг");
    CHECK(Read(data / "profiles" / again.profiles[1].id / "config.json").find("dropped") != std::string::npos);
  }
  CHECK(!fs::exists(data / "config.json"));
}

// 0.4.3's tray.json: one configuration in use and the server pick in it.
void TestFrom043(const fs::path& data) {
  Write(data / "tray.json",
        R"({"wantOn":true,"activeProfile":"p2","profiles":[)"
        R"({"id":"p1","name":"A","url":"","protocol":"x"},{"id":"p2","name":"B","url":"","protocol":"NL"}]})");
  const TraySettings settings = LoadSettings();
  CHECK(settings.profiles.size() == 2);
  if (settings.profiles.size() == 2) {
    CHECK(!settings.profiles[0].enabled && settings.profiles[1].enabled);
  }
  CHECK(settings.protocol == "NL");
  SaveSettings(settings);
  const auto saved = nlohmann::json::parse(Read(data / "tray.json"));
  CHECK(!saved.contains("activeProfile") && saved["protocol"] == "NL");
  CHECK(saved["profiles"][0]["enabled"] == false);
}

void TestFiles(const fs::path& data) {
  const fs::path a = ProfileDir("pa");
  const fs::path b = ProfileDir("pb");
  SaveConfig(a, "A");
  SaveConfig(b, "B");
  SavePending(a, "new");
  CHECK(LoadConfig(a) == std::optional<std::string>("A"));
  CHECK(LoadConfig(b) == std::optional<std::string>("B"));
  CHECK(LoadPending(b) == std::nullopt);
  ClearPending(a);
  ClearPending(a);  // none: not an error
  CHECK(LoadPending(a) == std::nullopt);
  SaveHistory(DataDir(), "removed");
  CHECK(fs::is_directory(data / "history"));
  RemoveProfileDir("pa");
  RemoveProfileDir("pa");  // gone: not an error
  CHECK(!fs::exists(a) && fs::exists(b));

  bool threw = false;
  try {
    (void)ProfileDir("..");
  } catch (...) {
    threw = true;
  }
  CHECK(threw);
}

}  // namespace

// `--migrate <dir>`: LoadSettings on a copy of a real %LOCALAPPDATA% (<dir>
// holds Sovereign\), and what came of it - no links, no keys.
int Migrate(const fs::path& localAppData) {
  SetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.c_str());
  const TraySettings settings = LoadSettings();
  for (const Profile& p : settings.profiles) {
    std::cout << p.id << (p.enabled ? " (on)" : " (off)") << ": " << p.name
              << (p.url.empty() ? ", own config" : ", subscription") << ", config.json "
              << (LoadConfig(ProfileDir(p.id)) ? "there" : "MISSING") << "\n";
  }
  const auto& r = settings.routing;
  std::cout << "routing: " << (r.source == RoutingSettings::Source::Own ? "own" : "profile")
            << (r.russiaDirect ? ", russia direct" : "") << (r.blockAds ? ", ads blocked" : "")
            << (r.blockQuic ? ", quic blocked" : "") << ", rules: " << r.rules.size() << "\n";
  for (const RouteRule& rule : r.rules) {
    std::cout << "  " << RuleText(rule) << " -> " << static_cast<int>(rule.action) << "\n";
  }
  return settings.profiles.empty() ? 1 : 0;
}

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  if (argc == 3 && std::string(argv[1]) == "--migrate") {
    return Migrate(argv[2]);
  }
  const fs::path root = fs::temp_directory_path() / "sovereign-settings-test";
  try {
    fs::remove_all(root);
    fs::create_directories(root);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    const fs::path data = root / "Sovereign";
    TestMigration(data);
    TestFrom043(data);
    TestFiles(data);
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
