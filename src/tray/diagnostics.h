#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "routing.h"

// The checks the "Проверка" page runs - what the internet sees, DNS and
// WebRTC leaks, IPv6, the local network, DNS latency, speed, where an
// address goes - the parts that don't touch the network: building and
// reading what goes over it, and judging the results. The runs themselves
// are in diagnose.cpp. Unit-tested in tests/unit/diagnostics_test.cpp, fuzzed
// in tests/fuzz/tray_input_fuzz.cpp.

namespace sovereign::tray {

struct CheckResult {
  enum class Status : std::uint8_t { NotRun, Running, Ok, Warn, Fail };
  Status status = Status::NotRun;
  std::string summary;  // one line: "5.83.147.210 · DE"
  std::string detail;   // what it means, what to do
};

// Cloudflare's /cdn-cgi/trace: "ip=...\nloc=DE\n...".
struct Trace {
  std::string ip;
  std::string loc;
};
std::optional<Trace> ParseTrace(std::string_view body);

// Yandex's ipv4-internet API: the address in quotes.
std::optional<std::string> ParseQuotedIp(std::string_view body);

// An address as text, IPv4 or IPv6, nothing else.
bool LooksLikeIp(std::string_view text);

// STUN (RFC 5389): a Binding Request with `id`, and the address a response
// to it carries (XOR-MAPPED-ADDRESS, else MAPPED-ADDRESS) - what a browser's
// WebRTC would tell a site.
using StunId = std::array<std::uint8_t, 12>;
std::string StunRequest(const StunId& id);
std::optional<std::string> ParseStunResponse(std::string_view bytes, const StunId& id);

// bash.ws's DNS leak test: the resolvers that asked for the test's names.
struct DnsResolverSeen {
  std::string ip;
  std::string country;  // "de"
  std::string org;      // "i3D.net B.V"
};
std::vector<DnsResolverSeen> ParseDnsLeak(std::string_view json);
// Judged: a resolver in Russia means names went out past the proxy - most
// likely the provider's DNS.
CheckResult JudgeDnsLeak(const std::vector<DnsResolverSeen>& resolvers);

// Where a connection went, from the core's log (info level): the outbound
// of the newest line about `targets` (the name, or one of its addresses).
std::optional<std::string> OutboundInLogs(const std::vector<std::string>& lines,
                                          const std::vector<std::string>& targets);

// Which of the client's rules - the user's, or Russia's zones - names `host`
// (lists aren't read: they're the core's), as a line for the user; empty if
// none does.
std::string ClientRuleFor(std::string_view host, const RoutingSettings& routing);

// Megabits per second for `bytes` in `seconds`.
double Mbps(std::uint64_t bytes, double seconds);

}  // namespace sovereign::tray
