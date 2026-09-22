#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Mirrors option.Outbound's envelope marshaling (sing-box's
// option/outbound.go: Outbound.MarshalJSONContext/UnmarshalJSONContext, via
// badjson.MarshallObjectsContext): the wire format is a single flat JSON
// object combining {"type", "tag"} with whatever fields the concrete
// Options type contributes — not a nested {"type":...,"options":{...}}
// shape. Go dispatches which Options type to parse into via a runtime
// registry (OutboundOptionsRegistry) keyed on "type"; we don't have that
// registry, so this template is instantiated per concrete Options type and
// the caller picks the right instantiation after inspecting "type" — see
// Sovereign issue #2. AnyTLS is the only outbound wired up so far; this is
// the pattern to repeat for the others.

namespace sovereign::adapters {

template <typename Options>
struct Outbound {
  std::string type;
  std::string tag;
  Options options{};
};

template <typename Options>
void to_json(nlohmann::json& j, const Outbound<Options>& v) {
  j = nlohmann::json::object();
  j["type"] = v.type;
  if (!v.tag.empty()) {
    j["tag"] = v.tag;
  }
  const nlohmann::json optionsJson = v.options;
  j.update(optionsJson);
}

template <typename Options>
void from_json(const nlohmann::json& j, Outbound<Options>& v) {
  v.type = j.at("type").get<std::string>();
  v.tag = j.contains("tag") ? j.at("tag").get<std::string>() : std::string{};
  v.options = j.get<Options>();
}

}  // namespace sovereign::adapters
