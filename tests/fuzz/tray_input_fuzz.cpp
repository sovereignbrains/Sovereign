// libFuzzer target: everything the tray does with text it didn't write - a
// subscription body and its headers from the server, the service's pipe
// responses, config.json on disk. None of it may throw or crash on any input
// (the worker thread has nowhere to catch), and the config rewriters must keep
// turning a JSON object into a JSON object.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "app_rules.h"
#include "cache_file.h"
#include "delays.h"
#include "log_lines.h"
#include "protocol_choice.h"
#include "subscription.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace sovereign::tray;
  const std::string_view text(reinterpret_cast<const char*>(data), size);

  (void)CheckSubscriptionConfig(text);
  (void)ConfigHasTun(text);
  (void)ParseUpdateInterval(text);
  (void)FindProtocolChoices(text);
  (void)ParseLogsResponse(text);
  (void)ParseDelaysResponse(text);
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
  return 0;
}
