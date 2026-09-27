// libFuzzer target: everything the tray does with text it didn't write - a
// subscription body and its headers from the server, the service's pipe
// responses, config.json on disk, GitHub's release answers. None of it may throw or crash on any input
// (the worker thread has nowhere to catch), and the config rewriters must keep
// turning a JSON object into a JSON object. The config merge also keeps its
// promises: one side unchanged gives the other side.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "app_rules.h"
#include "cache_file.h"
#include "config_sync.h"
#include "delays.h"
#include "log_lines.h"
#include "protocol_choice.h"
#include "subscription.h"
#include "update.h"

namespace {

// A config the merge runs against when the input plays one of its sides.
constexpr std::string_view kSample =
    R"({"outbounds":[{"type":"direct","tag":"direct"},{"type":"vless","tag":"nl","server":"a","server_port":443}],)"
    R"("route":{"rules":[{"action":"sniff"},{"ip_is_private":true,"outbound":"direct"}],"final":"nl"}})";

void CheckMerge(std::string_view base, std::string_view mine, std::string_view theirs) {
  using namespace sovereign::tray;
  (void)ClassifyArrival(std::string(base), std::string(mine), theirs);
  const auto merged = MergeConfigs(base, mine, theirs);
  const auto isObject = [](std::string_view t) {
    return nlohmann::json::parse(t, nullptr, /*allow_exceptions=*/false).is_object();
  };
  if (!merged) {
    const auto shallow = [](std::string_view t) { return NestingDepth(t) <= kMaxConfigDepth; };
    if (isObject(base) && isObject(mine) && isObject(theirs) && shallow(base) && shallow(mine) && shallow(theirs)) {
      std::abort();
    }
    return;
  }
  if (!isObject(merged->config)) {
    std::abort();
  }
  if ((SameConfig(base, theirs) && !SameConfig(merged->config, mine)) ||
      (SameConfig(base, mine) && !SameConfig(merged->config, theirs))) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace sovereign::tray;
  const std::string_view text(reinterpret_cast<const char*>(data), size);

  (void)CheckSubscriptionConfig(text);
  (void)ConfigHasTun(text);
  (void)ParseUpdateInterval(text);
  (void)FindProtocolChoices(text);
  (void)ParseLogsResponse(text);
  (void)ParseDelaysResponse(text);
  (void)ParseLatestRelease(text);
  (void)ParseChecksum(text, "Sovereign-Setup-1.2.3.exe");
  (void)ParseVersion(text);
  (void)IsHttpsUrl(std::wstring(text.begin(), text.end()));
  (void)UrlHost(std::wstring(text.begin(), text.end()));

  const bool object = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false).is_object();
  const std::vector<std::string> apps{"app.exe"};
  for (const std::string& rewritten :
       {ApplyProtocolChoice(text, "auto"), ApplyAppRules(text, AppsMode::Exclude, apps),
        ApplyAppRules(text, AppsMode::Include, apps), ApplyCacheFile(text, R"(C:\ProgramData\Sovereign\cache.db)")}) {
    if (object && !nlohmann::json::parse(rewritten, nullptr, /*allow_exceptions=*/false).is_object()) {
      std::abort();
    }
  }

  // The input as each side of a merge, and - cut in three at 0x1E - as all three.
  CheckMerge(kSample, text, kSample);
  CheckMerge(kSample, kSample, text);
  CheckMerge(text, kSample, kSample);
  if (const auto a = text.find('\x1e'); a != std::string_view::npos) {
    if (const auto b = text.find('\x1e', a + 1); b != std::string_view::npos) {
      CheckMerge(text.substr(0, a), text.substr(a + 1, b - a - 1), text.substr(b + 1));
    }
  }
  return 0;
}
