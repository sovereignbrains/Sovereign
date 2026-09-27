#include "config_sync.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <utility>

namespace sovereign::tray {

namespace {

// Ordered: the merged config keeps the subscription's key order ("type" and
// "tag" first), so it still reads like the subscription does.
using Json = nlohmann::ordered_json;

constexpr std::size_t kMaxConflicts = 100;
// Real configs are a few levels deep; anything past this is taken from the
// user's side whole instead of recursing further.
constexpr int kMaxDepth = 64;

// Equal JSON values, object key order aside (ordered_json's own == minds it).
bool Equal(const Json& a, const Json& b) {
  if (a.is_number() && b.is_number()) {
    return a == b;
  }
  if (a.type() != b.type()) {
    return false;
  }
  if (a.is_object()) {
    if (a.size() != b.size()) {
      return false;
    }
    for (const auto& item : a.items()) {
      const auto it = b.find(item.key());
      if (it == b.end() || !Equal(item.value(), *it)) {
        return false;
      }
    }
    return true;
  }
  if (a.is_array()) {
    if (a.size() != b.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (!Equal(a[i], b[i])) {
        return false;
      }
    }
    return true;
  }
  return a == b;
}

// nullptr: absent.
bool Same(const Json* a, const Json* b) { return a == nullptr || b == nullptr ? a == b : Equal(*a, *b); }

std::optional<Json> Copy(const Json* v) { return v != nullptr ? std::optional<Json>(*v) : std::nullopt; }

const Json* Member(const Json& object, const std::string& key) {
  const auto it = object.find(key);
  return it == object.end() ? nullptr : &*it;
}

std::string Join(const std::string& path, const std::string& key) { return path.empty() ? key : path + "." + key; }

// Arrays whose elements name themselves: all objects with a unique string "tag".
bool Tagged(const Json& array) {
  std::set<std::string> tags;
  for (const Json& element : array) {
    if (!element.is_object()) {
      return false;
    }
    const auto tag = element.find("tag");
    if (tag == element.end() || !tag->is_string() || !tags.insert(tag->get<std::string>()).second) {
      return false;
    }
  }
  return true;
}

// Each element's identity across the three versions: its tag, or its
// canonical text (sorted keys) and which occurrence of that text it is.
std::vector<std::string> Ids(const Json& array, bool tagged) {
  std::vector<std::string> ids;
  std::map<std::string, int> seen;
  for (const Json& element : array) {
    if (tagged) {
      ids.push_back("t:" + element.at("tag").get<std::string>());
    } else {
      std::string text = nlohmann::json(element).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
      const int occurrence = seen[text]++;
      ids.push_back("v:" + std::move(text) + "#" + std::to_string(occurrence));
    }
  }
  return ids;
}

std::map<std::string, std::size_t> Positions(const std::vector<std::string>& ids) {
  std::map<std::string, std::size_t> positions;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    positions.emplace(ids[i], i);
  }
  return positions;
}

const Json* Find(const std::map<std::string, std::size_t>& positions, const Json& array, const std::string& id) {
  const auto it = positions.find(id);
  return it == positions.end() ? nullptr : &array[it->second];
}

class Merger {
 public:
  std::vector<std::string> conflicts;

  // nullopt: absent in the result.
  std::optional<Json> Merge(const Json* base, const Json* mine, const Json* theirs, const std::string& path,
                            int depth) {
    if (Same(mine, theirs) || Same(theirs, base)) {
      return Copy(mine);
    }
    if (Same(mine, base)) {
      return Copy(theirs);
    }
    if (base != nullptr && mine != nullptr && theirs != nullptr && depth < kMaxDepth) {
      if (base->is_object() && mine->is_object() && theirs->is_object()) {
        return MergeObjects(*base, *mine, *theirs, path, depth + 1);
      }
      if (base->is_array() && mine->is_array() && theirs->is_array()) {
        return MergeArrays(*base, *mine, *theirs, path, depth + 1);
      }
    }
    Conflict(path);
    return Copy(mine);
  }

  Json MergeObjects(const Json& base, const Json& mine, const Json& theirs, const std::string& path, int depth) {
    // Theirs' key order, then the keys only the user has.
    std::vector<std::string> keys;
    for (const auto& item : theirs.items()) {
      keys.push_back(item.key());
    }
    for (const auto& item : mine.items()) {
      if (!theirs.contains(item.key())) {
        keys.push_back(item.key());
      }
    }
    Json out = Json::object();
    for (const std::string& key : keys) {
      if (auto merged = Merge(Member(base, key), Member(mine, key), Member(theirs, key), Join(path, key), depth)) {
        out[key] = std::move(*merged);
      }
    }
    return out;
  }

 private:
  void Conflict(const std::string& path) {
    const std::string where = path.empty() ? "(весь конфиг)" : path;
    if (conflicts.size() < kMaxConflicts && std::find(conflicts.begin(), conflicts.end(), where) == conflicts.end()) {
      conflicts.push_back(where);
    }
  }

  Json MergeArrays(const Json& base, const Json& mine, const Json& theirs, const std::string& path, int depth) {
    const bool tagged = Tagged(base) && Tagged(mine) && Tagged(theirs);
    if (!tagged) {
      // Both sides changed a list whose order matters (route rules) and
      // whose elements have no names: both changes are applied, but the
      // result is worth a look.
      Conflict(path);
    }
    const auto baseIds = Ids(base, tagged);
    const auto mineIds = Ids(mine, tagged);
    const auto theirIds = Ids(theirs, tagged);
    const auto baseAt = Positions(baseIds);
    const auto mineAt = Positions(mineIds);
    const auto theirAt = Positions(theirIds);
    const auto label = [&](const Json& element, std::size_t i) {
      return path + "[" + (tagged ? element.at("tag").get<std::string>() : std::to_string(i)) + "]";
    };

    // The new version's elements, merged with the user's side of each.
    std::vector<std::pair<std::string, Json>> out;
    for (std::size_t i = 0; i < theirs.size(); ++i) {
      const std::string& id = theirIds[i];
      if (auto merged = Merge(Find(baseAt, base, id), Find(mineAt, mine, id), &theirs[i], label(theirs[i], i), depth)) {
        out.emplace_back(id, std::move(*merged));
      }
    }
    // Then the user's elements the new version doesn't have: their additions,
    // and the ones they changed that the new version dropped - each after the
    // element it followed in the user's version.
    for (std::size_t i = 0; i < mine.size(); ++i) {
      const std::string& id = mineIds[i];
      if (theirAt.contains(id)) {
        continue;
      }
      auto merged = Merge(Find(baseAt, base, id), &mine[i], nullptr, label(mine[i], i), depth);
      if (!merged) {
        continue;
      }
      Json value = std::move(*merged);
      std::size_t at = 0;
      for (std::size_t j = i; j-- > 0;) {
        const auto it = std::find_if(out.begin(), out.end(), [&](const auto& e) { return e.first == mineIds[j]; });
        if (it != out.end()) {
          at = static_cast<std::size_t>(it - out.begin()) + 1;
          break;
        }
      }
      out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), {id, std::move(value)});
    }
    Json result = Json::array();
    for (auto& element : out) {
      result.push_back(std::move(element.second));
    }
    return result;
  }
};

}  // namespace

bool SameConfig(std::string_view a, std::string_view b) {
  const auto x = nlohmann::json::parse(a, nullptr, /*allow_exceptions=*/false);
  const auto y = nlohmann::json::parse(b, nullptr, /*allow_exceptions=*/false);
  if (x.is_discarded() || y.is_discarded()) {
    return a == b;
  }
  return x == y;
}

Arrival ClassifyArrival(const std::optional<std::string>& original, const std::optional<std::string>& config,
                        std::string_view fetched) {
  if (!config) {
    return Arrival::Replace;
  }
  const std::string& current = *config;
  const std::string& base = original ? *original : current;
  if (SameConfig(base, fetched)) {
    return Arrival::Unchanged;
  }
  if (SameConfig(current, base) || SameConfig(current, fetched)) {
    return Arrival::Replace;
  }
  return Arrival::Ask;
}

std::optional<MergedConfig> MergeConfigs(std::string_view base, std::string_view mine, std::string_view theirs) {
  const Json b = Json::parse(base, nullptr, /*allow_exceptions=*/false);
  const Json m = Json::parse(mine, nullptr, /*allow_exceptions=*/false);
  const Json t = Json::parse(theirs, nullptr, /*allow_exceptions=*/false);
  if (!b.is_object() || !m.is_object() || !t.is_object()) {
    return std::nullopt;
  }
  Merger merger;
  const Json merged = merger.MergeObjects(b, m, t, {}, 0);
  return MergedConfig{
      .config = merged.dump(2, ' ', false, Json::error_handler_t::replace) + "\n",
      .conflicts = std::move(merger.conflicts),
  };
}

}  // namespace sovereign::tray
