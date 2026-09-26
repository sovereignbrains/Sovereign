#pragma once

#include <string>
#include <string_view>
#include <vector>

// Which proxy the box uses: subscriptions (packetlab's among them) route
// through a selector outbound - "proxy": ["auto", "AnyTLS-REALITY", ...] -
// and sing-box starts it on its `default`. The tray lets the user pick that
// default; applied to the config it sends, like the per-app rules, so a new
// pick restarts the box through the config hash. Unit-tested in
// tests/unit/protocol_choice_test.cpp.

namespace sovereign::tray {

struct ProtocolChoices {
  std::string selector;              // the selector's tag; empty if the config has none
  std::vector<std::string> options;  // its outbounds, in the config's order
  std::string configDefault;         // its `default`, or the first option
};

// The selector route.final points at, else the first selector outbound.
ProtocolChoices FindProtocolChoices(std::string_view config);

// `config` with the selector's default set to `choice` - unchanged (but
// re-dumped, as the service hashes it) if `choice` is empty or not one of the
// options, e.g. after the subscription dropped a protocol.
std::string ApplyProtocolChoice(std::string_view config, const std::string& choice);

}  // namespace sovereign::tray
