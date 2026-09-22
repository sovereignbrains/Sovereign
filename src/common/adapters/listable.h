#pragma once

#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// Mirrors github.com/sagernet/sing/common/json/badoption.Listable[T]:
// Go marshals a single-element list as the bare element (not `[x]`), and
// any other length (0 or >1) as a JSON array. Unmarshal tries a bare value
// first, falling back to an array — see badoption/listable.go. We only
// implement the array-vs-scalar branch (is_array()), which is equivalent
// for every T actually used in sing-box's option package (T is never
// itself array-shaped there); a T that were array-shaped would need the
// same try-single-then-fallback logic badoption.go uses.

namespace sovereign::adapters {

template <typename T>
class Listable {
 public:
  Listable() = default;
  Listable(std::vector<T> values) : values(std::move(values)) {}  // NOLINT(google-explicit-constructor)

  std::vector<T> values;
};

template <typename T>
void to_json(nlohmann::json& j, const Listable<T>& l) {
  if (l.values.size() == 1) {
    j = l.values.front();
  } else {
    j = l.values;
  }
}

template <typename T>
bool IsEmptyValue(const Listable<T>& l) {
  return l.values.empty();
}

template <typename T>
void from_json(const nlohmann::json& j, Listable<T>& l) {
  l.values.clear();
  if (j.is_array()) {
    l.values = j.get<std::vector<T>>();
  } else if (!j.is_null()) {
    l.values.push_back(j.get<T>());
  }
}

}  // namespace sovereign::adapters
