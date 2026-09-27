#pragma once

#include <string>
#include <string_view>

// sing-box's cache file (experimental.cache_file), always on and always at a
// path the tray picks. Its point here: remote rule-sets (the subscription's ad
// filter) are kept between runs, so the box starts from the cached copy when
// the download fails - without it, one unreachable rule-set URL (no network
// yet after a resume, the subscription server down) stops the whole box from
// starting ("initialize rule-set: ... context deadline exceeded"). The path is
// absolute because the box runs inside the service, whose working directory is
// System32. Unit-tested in tests/unit/cache_file_test.cpp.

namespace sovereign::tray {

// `config` with experimental.cache_file enabled at `path` (UTF-8). Whatever
// else the subscription set in cache_file stays; a path of its own is
// replaced. The result is nlohmann's dump (what the service hashes); text that
// isn't a JSON object comes back unchanged.
std::string ApplyCacheFile(std::string_view config, const std::string& path);

}  // namespace sovereign::tray
