#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// Cloudflare WARP as a way out of the box: a free WARP device registered the
// way the official client does it (a WireGuard key made here, the public half
// sent to Cloudflare's API), and the sing-box WireGuard endpoint built from
// what came back. Routing sends traffic to it (routing.h): rules with "через
// WARP", IPv6. The request goes out from the tray (fetch.h); this part has
// no network and no Win32 - unit-tested in tests/unit/warp_test.cpp.

namespace sovereign::tray {

struct WarpAccount {
  std::string privateKey;  // base64 of 32 bytes: WireGuard's, never sent anywhere
  std::string id;          // the device, as Cloudflare names it
  std::string token;       // its API token (to change or delete it later)
  std::string address4;    // "172.16.0.2"
  std::string address6;    // "2606:4700:110:8a1f:..."; may be empty
  std::string peerKey;     // Cloudflare's WireGuard key, base64
  std::string host = "engage.cloudflareclient.com";
  int port = 2408;
  std::array<int, 3> reserved{};  // from client_id: Cloudflare tells devices apart by it
};

// Where a device is registered, and as what client.
inline constexpr std::string_view kWarpRegisterUrl = "https://api.cloudflareclient.com/v0a2158/reg";
inline constexpr std::string_view kWarpUserAgent = "okhttp/3.12.1";
inline constexpr std::string_view kWarpClientVersion = "a-6.10-2158";  // CF-Client-Version
inline constexpr int kWarpKeepaliveSeconds = 25;  // WireGuard's usual behind NAT (WarpEndpoint)

// The registration's body: the public key (base64) and the time the terms
// were accepted ("2026-10-02T12:00:00.000Z").
std::string WarpRegisterBody(std::string_view publicKey, std::string_view tos);

// The account in the API's answer, with the private key it was asked with;
// nullopt if something it needs isn't there or isn't what it should be.
std::optional<WarpAccount> ParseWarpRegistration(std::string_view response, std::string privateKey);

// tray.json's "warpAccount", read defensively.
nlohmann::json WarpToJson(const WarpAccount& account);
std::optional<WarpAccount> WarpFromJson(const nlohmann::json& json);

// The sing-box WireGuard endpoint for `account`, tagged `tag`; through
// `detour` (an outbound's tag) when it isn't empty - WARP over the proxy.
nlohmann::ordered_json WarpEndpoint(const WarpAccount& account, std::string_view tag, std::string_view detour);

}  // namespace sovereign::tray
