#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The config the box runs when several configurations are on at once
// (profiles.h), and servers switched off. The first configuration on gives
// the whole frame - its inbounds, DNS, routing, rule sets - with its servers
// less the ones switched off; every other one adds its servers (outbounds and
// endpoints, not its groups, DNS or rules) to the first one's proxy selector
// and to the URL tests in it, so "auto" and the server list cover them all.
// A server switched off takes with it what can't work without it - outbounds
// chained through it (detour), groups left empty - and what pointed at it
// (route.final, rules, DNS detours, rule-set downloads) points at the proxy
// selector instead. No Win32 here: unit-tested, the results through the
// pinned sing-box's `check`, in tests/unit/combine_test.cpp.

namespace sovereign::tray {

// One configuration's part.
//
// `via`: the id of another part its servers connect through - a chain, so
// this part's servers see the other's address, never the user's. The other
// part's servers go into a URL test of their own ("«<its name>»", not
// offered in the selector), and every server of this part that isn't
// already chained within the part gets it as its detour. A chain that can't
// be made - that part is off, missing, chained itself, or this one - drops
// this part's servers with a note rather than let them connect directly.
struct ProfileConfig {
  std::string name;                   // for its servers' tags that clash with ones before: "NL · <name>"
  std::string config;                 // its config.json
  std::vector<std::string> disabled;  // tags of its servers switched off
  std::string id{};                   // what another part's `via` names it by
  std::string via{};                  // the part to connect through; empty: directly
};

struct CombinedConfig {
  std::optional<std::string> config;  // nullopt: nothing to run - `error` says why
  std::string error;
  std::vector<std::string> notes;  // what was left out on the way ("B: не JSON")
};

// `parts` in the order they're listed; the first is the frame - unless
// `ownFrame` is given (routing.h's OwnFrame: the client's own TUN, DNS and
// routing), then every part only gives servers. One part with nothing
// switched off and no own frame comes back as it is.
CombinedConfig CombineConfigs(const std::vector<ProfileConfig>& parts,
                              const std::optional<std::string>& ownFrame = std::nullopt);

// A server of a config, as the window lists it.
struct ServerInfo {
  std::string tag;
  std::string type;   // sing-box's: "vless", "wireguard"...
  std::string label;  // "VLESS · REALITY", "Trojan · WS", "WireGuard"
};

// The servers in a config's outbounds and endpoints (not groups, direct,
// block, dns), in its order. Empty if it isn't a JSON object.
std::vector<ServerInfo> ListServers(std::string_view config);

}  // namespace sovereign::tray
