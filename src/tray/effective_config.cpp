#include "effective_config.h"

#include "cache_file.h"
#include "log_level.h"
#include "protocol_choice.h"

namespace sovereign::tray {

std::string EffectiveConfig(std::string_view config, const ConfigAdditions& additions) {
  std::string effective = ApplyLogLevel(
      ApplyAppRules(ApplyProtocolChoice(config, additions.protocol), additions.appsMode, additions.apps),
      additions.logLevel);
  return additions.cacheFile.empty() ? effective : ApplyCacheFile(effective, additions.cacheFile);
}

}  // namespace sovereign::tray
