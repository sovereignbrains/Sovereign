#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sovereign::tray {

// One server's latency as the service's box_delays reports it.
struct Delay {
  enum class State : std::uint8_t { Pending, Ok, Failed };
  State state = State::Pending;
  int ms = 0;           // Ok: the median round trip
  int jitter = 0;       // Ok: half the spread of the round trips
  int loss = 0;         // Ok: percent of requests lost
  int samples = 0;      // Ok: round trips measured
  int connect = 0;      // a connection, TLS and a first request; 0 if unknown
  std::string error{};  // Failed
  int failedInRow = 0;  // the tray's count, not the service's: tests in a row this server failed
};

// Results by outbound tag; nullopt if `response` isn't a box_delays answer
// (an error, or garbage). Entries that aren't a result are skipped.
std::optional<std::map<std::string, Delay>> ParseDelaysResponse(std::string_view response);

// Which sites open through a server, as the service's box_sites reports it
// (gocore/sites.go): one verdict per site id ("youtube", "telegram", ...).
struct SiteCheck {
  enum class State : std::uint8_t { Ok, Blocked, Fail };  // Blocked: it answered, but not to this exit
  std::string site;
  State state = State::Fail;
};
struct SiteChecks {
  bool pending = false;  // a check runs; `sites` may be partial
  std::string error;     // why there is nothing (an older core, box not running...)
  std::vector<SiteCheck> sites;
};

// nullopt unless it's a box_sites answer. Entries whose id isn't a short
// lowercase word are dropped; an unknown state reads as a failure.
std::optional<SiteChecks> ParseSitesResponse(std::string_view response);

// What a measured server costs, in milliseconds: the median, plus its spread
// and its losses - a jumpy or lossy server is worse than its median says.
// Only for State::Ok.
int DelayScore(const Delay& delay);

// The server auto runs on, and the one about to take over from it.
struct AutoPick {
  std::string server{};     // empty: none picked yet
  std::string candidate{};  // better than `server` in the last test
  int wins = 0;             // tests in a row the candidate has won
};

// Auto's pick after a latency test: the best of `members` by DelayScore when
// there is none yet or the one in use failed; otherwise a better one only
// once it has won two tests in a row by a clear margin (a fifth, and 20 ms) -
// the measurements of two servers a few milliseconds apart swap places from
// test to test, and switching between them would only drop connections.
// Servers not measured in this test keep the pick as it is.
AutoPick JudgeAuto(AutoPick pick, const std::vector<std::string>& members,
                   const std::map<std::string, Delay>& delays);

// The user's own network, judged by the way around the proxy - the config's
// direct outbound, measured with the servers to the same site: slow or lossy
// there and the servers' numbers are the Wi-Fi's or the ISP's, not theirs.
// Slow: 100 ms or more, and (when there's a best seen) 2.5 times it and 60 ms
// over it; lossy: 30% or more lost; down: it failed and so did every server.
struct LocalNetwork {
  enum class State : std::uint8_t { Fine, Slow, Lossy, Down };
  State state = State::Fine;
  std::string forUser;     // a sentence for the main screen; empty when fine
  std::string forSupport;  // a phrase for the support report: "not your servers"
};
LocalNetwork JudgeLocalNetwork(const Delay& direct, int failedInRow, std::optional<int> bestMs, bool serversDown);

// The server WARP goes through next, its present one not getting it to
// Cloudflare: of `servers` the ones not `tried` yet - those that answered
// the last test first, best by DelayScore, then the ones not measured, the
// ones that failed last. Empty when every one was tried.
std::string NextWarpServer(const std::vector<std::string>& servers, const std::vector<std::string>& tried,
                           const std::map<std::string, Delay>& delays);

}  // namespace sovereign::tray
