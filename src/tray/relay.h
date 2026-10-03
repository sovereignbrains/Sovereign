#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The subscription relay's protocol (tools/relay/worker.js), apart from
// WinHTTP (fetch.h) so it can be unit-tested (tests/unit/relay_test.cpp).
//
// The relay is a Cloudflare Worker in the user's own account: with no
// connection up, a subscription is fetched through it, so its server sees
// Cloudflare's address instead of the user's. POST <relay> with X-Relay-Key
// and RelayRequestBody gets the subscription back - whole when small; when
// big, only a job (X-Relay-Job, -Size, -Chunk) whose pieces come from
// GET <relay>chunk?job=..&n=.., each over a connection of its own: Russian
// networks cut connections to Cloudflare after about 24 KB.

namespace sovereign::tray {

// The relay's address: https, a host, no query or fragment. The key: what
// the worker's KEY secret is - printable ASCII without spaces, 16..256 long.
bool IsRelayUrl(std::string_view url);
bool IsRelayKey(std::string_view key);

// The address with one trailing slash ("https://x.workers.dev/"), what the
// POST goes to and the pieces' paths are relative to.
std::string RelayBase(std::string_view url);

// The POST's JSON: the subscription's URL and the headers the relay sends on
// to its server - the tray's User-Agent, and x-hwid when it is one.
std::string RelayRequestBody(std::string_view url, std::string_view userAgent, std::string_view hwid);

// A big answer's pieces, from the headers that announce them. nullopt when
// they don't make sense: a job that isn't a UUID, sizes that aren't numbers,
// more than maxBytes, or more pieces than kMaxRelayPieces.
inline constexpr std::size_t kMaxRelayPieces = 512;
struct RelayPlan {
  std::string job;
  std::size_t size = 0;
  std::size_t chunk = 0;
  std::size_t count = 0;  // pieces: size / chunk, rounded up
};
std::optional<RelayPlan> ParseRelayPlan(std::string_view job, std::string_view size, std::string_view chunk,
                                        std::size_t maxBytes);

// Piece n's address, relative to RelayBase: "chunk?job=<job>&n=<n>".
std::string RelayChunkPath(const RelayPlan& plan, std::size_t n);

}  // namespace sovereign::tray
