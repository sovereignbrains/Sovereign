#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The config and the subscription it came from. The subscription arrives
// whole and is kept as it arrived (subscription.json); config.json starts as
// a copy and is the user's to edit. A newer subscription replaces an
// unedited config silently; over an edited one the user chooses: take the new
// one, keep theirs, or carry their edits over to the new one (MergeConfigs).
// The pure part, apart from the files and the worker - unit-tested in
// tests/unit/config_sync_test.cpp.

namespace sovereign::tray {

// Whether two configs say the same thing: equal JSON values - key order,
// spacing and number spelling don't count. Text that isn't JSON compares as
// text.
bool SameConfig(std::string_view a, std::string_view b);

enum class Arrival : std::uint8_t {
  Unchanged,  // what the config is based on: nothing to do
  Replace,    // the config has no edits of its own: the new one becomes it
  Ask,        // the user edited the config: the new one waits for their choice
};

// What to do with a freshly fetched subscription. `original` is the one the
// config is based on - nullopt before the first, or for a config from before
// subscriptions were kept (then config.json is taken for the original).
Arrival ClassifyArrival(const std::optional<std::string>& original, const std::optional<std::string>& config,
                        std::string_view fetched);

struct MergedConfig {
  std::string config;  // pretty-printed JSON
  // Where both sides changed the same thing: "outbounds[proxy].server",
  // "route.rules". The user's side won in each; they're shown so the user
  // can look.
  std::vector<std::string> conflicts;
};

// A three-way merge: the user's edits (`base` -> `mine`) carried over to the
// subscription's new version (`base` -> `theirs`). Objects merge key by key;
// arrays whose elements all carry a unique "tag" (outbounds, rule sets, DNS
// servers) merge element by element; other arrays (route rules) keep the
// new version's elements with the user's additions and removals applied,
// the additions after the element they followed. nullopt if any of the
// three isn't a JSON object.
std::optional<MergedConfig> MergeConfigs(std::string_view base, std::string_view mine, std::string_view theirs);

}  // namespace sovereign::tray
