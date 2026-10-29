#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

// Which proxy the box uses: subscriptions (packetlab's among them) route
// through a selector outbound - "proxy": ["auto", "AnyTLS-REALITY", ...] -
// and sing-box starts it on its `default` (or on what its cache file kept).
// The tray switches the running selector to the user's pick (box_select)
// rather than restart the box with another default; for an "auto" option it
// picks the server itself, from its own measurements (delays.h). Unit-tested
// in tests/unit/protocol_choice_test.cpp.

namespace sovereign::tray {

struct ProtocolChoices {
  std::string selector;              // the selector's tag; empty if the config has none
  std::vector<std::string> options;  // its outbounds, in the config's order
  std::string configDefault;         // its `default`, or the first option
  // The options that are URL tests ("auto"): their servers, the ones of
  // them that are options too.
  std::map<std::string, std::vector<std::string>> groups;
};

// The selector route.final points at, else the first selector outbound.
ProtocolChoices FindProtocolChoices(std::string_view config);

// `config` with the selector's default set to `choice` (for a box started
// without the tray to steer it - the conformance harness) - unchanged (but
// re-dumped, as the service hashes it) if `choice` is empty or not one of the
// options, e.g. after the subscription dropped a protocol.
std::string ApplyProtocolChoice(std::string_view config, const std::string& choice);

// What a server is, from its outbound - not from the name a subscription
// gave it: "AnyTLS + REALITY", "Hysteria 2", "VLESS + Vision + REALITY",
// "VLESS + WebSocket + TLS", "Shadowsocks 2022"; a URL test is "Авто".
struct ServerTitle {
  std::string title;
  std::string host;  // where it connects ("de1.example.com"), for telling apart two of one title
  int port = 0;
};
// By tag, for every outbound and endpoint it can tell (an unknown type, a
// selector: none - shown by its tag).
std::map<std::string, ServerTitle> ServerTitles(std::string_view config);

// Whether a server's name says no more than its title: "AnyTLS-REALITY" for
// "AnyTLS + REALITY", "hysteria2" for "Hysteria 2" - the same letters and
// digits, case and the rest aside - or a name of protocol words, a country's
// two letters and numbers alone ("REALITY", "NL-Naive", "AnyTLS-REALITY 2").
bool NameIsTitle(std::string_view name, std::string_view title);

}  // namespace sovereign::tray
