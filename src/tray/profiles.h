#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// The configurations the tray keeps - one per subscription or per set of
// keys, a config file - and which one is in use: adding one never replaces
// another. Each has a folder of its own, profiles\<id>\ (settings.h), with
// its config.json, the subscription it's based on and its history; the
// per-config choices (the selector's pick) live with it here. The pure part,
// apart from the files and the worker - unit-tested in
// tests/unit/profiles_test.cpp.

namespace sovereign::tray {

struct Profile {
  std::string id;             // its folder's name: [a-z0-9-], at most kMaxProfileId
  std::string name;           // what the window shows (UTF-8)
  std::string url;            // the subscription (UTF-8); empty = a config of the user's own
  std::int64_t lastRefresh = 0;  // unix seconds of the last successful refresh
  int updateHours = 12;          // from Profile-Update-Interval, else the default
  std::string protocol;          // the proxy selector's option to use; empty = the config's (protocol_choice.h)
};

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

// Which profile is in use once `removed` is gone: the same one if it wasn't
// the removed one, else the one after it (or before, if it was last); empty
// when none is left.
std::string ActiveAfterRemoval(const std::vector<Profile>& profiles, std::string_view active,
                               std::string_view removed);

// tray.json's "profiles": entries with a bad id, a repeated id or the wrong
// types are dropped, the rest kept - one broken entry doesn't cost the others.
nlohmann::json ProfilesToJson(const std::vector<Profile>& profiles);
std::vector<Profile> ProfilesFromJson(const nlohmann::json& json);

}  // namespace sovereign::tray
