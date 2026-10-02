#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// The configurations the tray keeps - one per subscription or per set of
// keys, a config file - each on or off: all that are on run together
// (combine.h), and adding one never replaces another. Each has a folder of
// its own, profiles\<id>\ (settings.h), with its config.json, the
// subscription it's based on and its history. The pure part, apart from the
// files and the worker - unit-tested in tests/unit/profiles_test.cpp.

namespace sovereign::tray {

struct Profile {
  std::string id;             // its folder's name: [a-z0-9-], at most kMaxProfileId
  std::string name;           // what the window shows (UTF-8); the user may rename it
  bool userNamed = false;     // the user named it: Profile-Title no longer does
  std::string url;            // the subscription (UTF-8); empty = a config of the user's own
  bool enabled = true;        // its servers are in the config the box runs
  std::int64_t lastRefresh = 0;  // unix seconds of the last successful refresh
  int updateHours = 12;          // from Profile-Update-Interval, else the default
  bool autoUpdate = true;        // refreshed on its interval; off: only by hand
  int userHours = 0;             // the interval the user set; 0 = the server's (updateHours)
  std::vector<std::string> disabled;  // tags of its servers switched off
  std::string hwid;  // the x-hwid its subscription gets: random, its own (IsHwid); empty until the first fetch
};

// What a subscription server is told the device is (x-hwid, which panels
// with a device limit want): random per configuration, never the machine's -
// two subscriptions can't be tied together by it, and a refresh is the same
// device to the panel. 32 hex digits, as the random bytes are made by the tray.
bool IsHwid(std::string_view hwid);

// The hosts of the subscriptions (names or address literals, without port),
// each once: the tray's rules send them through the proxy, so a panel sees
// the proxy's address, not the user's.
std::vector<std::string> SubscriptionHosts(const std::vector<Profile>& profiles);

// How often a subscription is refreshed: the user's interval, else the server's.
inline int RefreshHours(const Profile& p) { return p.userHours > 0 ? p.userHours : p.updateHours; }
inline constexpr int kMaxRefreshHours = 24 * 30;

inline constexpr std::size_t kMaxProfileId = 40;
inline constexpr std::size_t kMaxProfileName = 64;  // characters, not bytes

// An id that can name a folder: nothing that could leave profiles\ or clash
// with a device name. tray.json is a file on disk - an id from it is
// checked before it's used as a path.
bool IsProfileId(std::string_view id);

// A fresh id, unused among `profiles`: "p" and `seed` (the time), numbered on.
std::string NewProfileId(const std::vector<Profile>& profiles, std::int64_t seed);

// `base` if no profile has that name, else "base 2", "base 3"...
std::string UniqueProfileName(const std::vector<Profile>& profiles, std::string_view base);

// A name fit to show: control characters out, spaces trimmed, at most
// kMaxProfileName characters (cut on a UTF-8 boundary). Empty if nothing's left.
std::string CleanProfileName(std::string_view name);

// The Profile-Title header subscription panels send (Marzban, Remnawave,
// 3x-ui): plain text, or "base64:" and base64 of UTF-8. Cleaned; nullopt
// when there's no usable name in it.
std::optional<std::string> ParseProfileTitle(std::string_view header);

// A new profile's name before anything better is known: the subscription
// server's host ("sub.example.com"), or "Мой конфиг" for one without a link.
std::string DefaultProfileName(std::string_view url);

const Profile* FindProfile(const std::vector<Profile>& profiles, std::string_view id);
Profile* FindProfile(std::vector<Profile>& profiles, std::string_view id);
const Profile* FindProfileByUrl(const std::vector<Profile>& profiles, std::string_view url);

// A server switched on or off in `profile` (by its tag).
void SetServerEnabled(Profile& profile, const std::string& tag, bool enabled);
bool IsServerEnabled(const Profile& profile, std::string_view tag);

// tray.json's "profiles": entries with a bad id, a repeated id or the wrong
// types are dropped, the rest kept - one broken entry doesn't cost the others.
// A list from before profiles could be on together had one in use,
// `legacyActive`: an entry without "enabled" is on if it's that one (or if
// there was none).
nlohmann::json ProfilesToJson(const std::vector<Profile>& profiles);
std::vector<Profile> ProfilesFromJson(const nlohmann::json& json, std::string_view legacyActive = {});

}  // namespace sovereign::tray
