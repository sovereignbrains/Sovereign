#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

#include "share_links.h"

// Other clients' formats for share_links.cpp: their servers written as share
// links (so one parser turns every server into an outbound) or, for sing-box's
// own outbounds, taken as they are. Not for use outside share_links.cpp.

namespace sovereign::tray::detail {

using Json = nlohmann::ordered_json;

// Clash/Mihomo (proxies, proxy-providers - as JSON too), Xray/V2Ray configs
// and v2rayN's list of them, SIP008, sing-box outbounds without a config
// around them. False, with `items` untouched, when `document` is none of these
// or lists no server - a whole sing-box config among them.
bool ImportJson(const Json& document, ImportItems& items);

// A line that starts "proxies:" or "proxy-providers:".
bool LooksLikeClashYaml(std::string_view text);
bool ImportYaml(std::string_view text, ImportItems& items);

}  // namespace sovereign::tray::detail
