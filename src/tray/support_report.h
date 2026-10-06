#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The report for a subscription's support (P6, #8): what's wrong with which
// of its servers, in words a provider's support reads without asking back -
// "the server turns the key away: the subscription is likely out of date",
// not "it doesn't work". Built from what the tray already measures (delays,
// exits, the checks) and what the panel said (traffic, the paid period).
//
// Nothing secret goes in: never the subscription's link (its token) - only
// its host; never a key, UUID or password; never the user's address -
// errors quoting it are cut. The pure part, unit-tested in
// tests/unit/support_report_test.cpp.

namespace sovereign::tray {

struct ReportServer {
  std::string name;      // as the user sees it: "🇳🇱 Netherlands"
  std::string protocol;  // "anytls · REALITY", "vless · ws · TLS"
  std::string host;      // the server's address or name, with no credentials
  int port = 0;
  enum class State : std::uint8_t { Unknown, Ok, Failed };
  State state = State::Unknown;
  int delay = 0;      // Ok: ms
  int jitter = 0;     // Ok: ms
  int loss = 0;       // Ok: percent
  std::string error;  // Failed: the core's words
  std::string exitIp;       // where it comes out, when known
  std::string exitCountry;  // "NL"
  std::string exitIsp;
};

// Only what's about the provider's service goes in: nothing of the client,
// the system or the user's network (country, ISP, leak checks) - none of it
// helps them fix a server, all of it tells them about the user. Dates in UTC.
struct ReportInput {
  std::int64_t now = 0;           // unix seconds
  std::string subscriptionName;   // "packetlab.tech"
  std::string subscriptionHost;   // the link's host only
  std::int64_t lastRefresh = 0;   // 0: never
  std::string refreshError;       // the last refresh's, if it failed
  std::uint64_t trafficUsed = 0;
  std::uint64_t trafficTotal = 0;  // 0: no limit / not sent
  std::int64_t expire = 0;         // 0: no end / not sent
  bool connected = false;          // the box runs
  std::vector<ReportServer> servers;
  std::string userIp;  // only to be cut out of anything quoted; never written
};

struct SupportReport {
  std::string full;   // the file
  std::string brief;  // a few lines for a chat
};

// What's wrong with a server, as a sentence for support - empty when nothing
// is (it works, its exit matches its name).
std::string ServerProblem(const ReportServer& server);

// The country a name's flag emoji names ("🇳🇱 Amsterdam" -> "NL"), or empty.
std::string FlagCountry(std::string_view name);

// `text` with the user's address and any private/local one (the PC's own,
// "192.168.31.57:5555") replaced by "<...>".
std::string CutAddresses(std::string_view text, std::string_view userIp);

// The servers of a sing-box config for the report, in its order: each
// outbound or endpoint that dials a server (not groups, direct, block, DNS),
// with its protocol chain and address. No credentials are read.
std::vector<ReportServer> ReportServers(std::string_view config);

SupportReport BuildSupportReport(const ReportInput& input);

// What's wrong with a subscription, for the main screen - empty while it's
// fine: the paid period over, the traffic used up, half its servers or more
// not connecting (`failed` of the `measured` of its `servers`), it not
// refreshing. The worst one only.
std::string SubscriptionTrouble(std::size_t servers, std::size_t measured, std::size_t failed, bool refreshFailed,
                                std::uint64_t used, std::uint64_t total, std::int64_t expire, std::int64_t now);

// The subscription card's line from what the panel said: "Трафик: 3 ГБ из
// 100,0 ГБ · оплачено до 01.01.2027" ("истекло" once past); empty if it said
// nothing.
std::string UsageLine(std::uint64_t used, std::uint64_t total, std::int64_t expire, std::int64_t now,
                      int utcOffsetMinutes);

}  // namespace sovereign::tray
