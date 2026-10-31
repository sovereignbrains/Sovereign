#include "go_core.h"

#include <wil/result.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace sovereign::service {

namespace {

// GetProcAddress always returns FARPROC; casting it to the export's real
// signature is the only way to call it and is inherent to the Win32 API
// shape. The intermediate uintptr_t cast (rather than a direct
// FARPROC->target reinterpret_cast) is deliberate: clang's
// cast-function-type-mismatch diagnostic is a hard error here (not a
// suppressible lint), and going through an integer breaks the pattern it
// matches on.
template <typename Fn>
Fn ResolveExport(HMODULE module, const char* name) {
  const FARPROC proc = GetProcAddress(module, name);
  THROW_LAST_ERROR_IF(!proc);
  return reinterpret_cast<Fn>(  // NOLINT(performance-no-int-to-ptr)
      reinterpret_cast<std::uintptr_t>(proc));
}

// Windows keeps what it resolved - an address from before the rules changed,
// a fake one the stopped box handed out, a failure from while it was down - and
// the site stays shut until that answer expires (what `ipconfig /flushdns`
// cures). Called after every start and stop of the box, so a new rule or a
// new way takes hold at once. Best effort: dnsapi's export isn't documented,
// and a missing one or a stopped DNS client service leaves things as they
// were - the answers just expire on their own.
void FlushSystemDnsCache() noexcept {
  using FlushFn = BOOL(WINAPI*)();
  const HMODULE dnsapi = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (dnsapi == nullptr) {
    return;
  }
  if (const FARPROC proc = GetProcAddress(dnsapi, "DnsFlushResolverCache")) {
    reinterpret_cast<FlushFn>(  // NOLINT(performance-no-int-to-ptr)
      reinterpret_cast<std::uintptr_t>(proc))();
  }
  FreeLibrary(dnsapi);
}

}  // namespace

std::wstring ResolveGoCoreDllPath() {
  wchar_t path[MAX_PATH]{};
  THROW_LAST_ERROR_IF(GetModuleFileNameW(nullptr, path, MAX_PATH) == 0);

  std::wstring exePath(path);
  const size_t lastSlash = exePath.find_last_of(L'\\');
  const std::wstring dir = (lastSlash == std::wstring::npos) ? L"." : exePath.substr(0, lastSlash);
  return dir + L"\\sovereign-gocore.dll";
}

GoCore::GoCore(const std::wstring& dllPath) {
  // LoadLibraryW with an absolute path we computed ourselves (never a
  // bare filename) — deliberately sidesteps Windows' default DLL search
  // order, which would otherwise probe the current working directory and
  // other locations an attacker could plant a DLL in.
  module_.reset(LoadLibraryW(dllPath.c_str()));
  THROW_LAST_ERROR_IF(!module_);
  // Pinned: a Go c-shared DLL can't be unloaded - its runtime's threads keep
  // running code inside it after any call returns (golang/go#11100), so the
  // FreeLibrary module_'s destructor would do unmaps code under them and the
  // process dies at some random moment later (0xC0000005; seen as a flaky
  // SegFault of the conformance harness in CI, db5401c, and 1 in 60 runs
  // locally). A pinned module stays until the process ends, whatever
  // FreeLibrary says.
  HMODULE pinned = nullptr;
  THROW_IF_WIN32_BOOL_FALSE(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, dllPath.c_str(), &pinned));

  boxPing_ = ResolveExport<BoxPingFn>(module_.get(), "box_ping");
  boxStart_ = ResolveExport<BoxStartFn>(module_.get(), "box_start");
  boxStop_ = ResolveExport<BoxStopFn>(module_.get(), "box_stop");
  boxStats_ = ResolveExport<BoxStatsFn>(module_.get(), "box_stats");
  boxFlows_ = ResolveExport<BoxFlowsFn>(module_.get(), "box_flows");
  boxRoute_ = ResolveExport<BoxRouteFn>(module_.get(), "box_route");
  boxUrlTest_ = ResolveExport<BoxUrlTestFn>(module_.get(), "box_urltest");
  boxDelays_ = ResolveExport<BoxDelaysFn>(module_.get(), "box_delays");
  boxExitIp_ = ResolveExport<BoxExitIpFn>(module_.get(), "box_exitip");
  if (const FARPROC sites = GetProcAddress(module_.get(), "box_sites")) {
    boxSites_ = reinterpret_cast<BoxSitesFn>(  // NOLINT(performance-no-int-to-ptr)
        reinterpret_cast<std::uintptr_t>(sites));
  }
  boxSelect_ = ResolveExport<BoxSelectFn>(module_.get(), "box_select");
  boxSetLogCallback_ = ResolveExport<BoxSetLogCallbackFn>(module_.get(), "box_set_log_callback");
  boxFree_ = ResolveExport<BoxFreeFn>(module_.get(), "box_free");

  // Registered once for the object's lifetime; SetLogSink only swaps what
  // OnLog forwards to, so installing a sink never has to cross into Go.
  boxSetLogCallback_(&GoCore::OnLog, this);
}

GoCore::~GoCore() { boxSetLogCallback_(nullptr, nullptr); }

void __cdecl GoCore::OnLog(void* context, int level, const char* message) noexcept {
  auto* self = static_cast<GoCore*>(context);
  try {
    const std::scoped_lock lock(self->sinkMutex_);
    if (self->sink_) {
      self->sink_(static_cast<LogLevel>(level), message);
    }
  } catch (...) {
    // A sink failure must not unwind into Go's stack (undefined behavior
    // across the cgo boundary); a lost log line is the lesser evil. WIL
    // reports it (to ETW in the service, see trace.h).
    LOG_CAUGHT_EXCEPTION_MSG("log sink threw, line dropped");
  }
}

void GoCore::SetLogSink(LogSink sink) {
  const std::scoped_lock lock(sinkMutex_);
  sink_ = std::move(sink);
}

std::string GoCore::TakeOwnedString(char* dllString) {
  if (!dllString) {
    return {};
  }
  std::string result(dllString);
  boxFree_(dllString);
  return result;
}

std::string GoCore::Ping() { return TakeOwnedString(boxPing_()); }

std::string GoCore::Start(const std::string& configJson) {
  std::string error = TakeOwnedString(boxStart_(configJson.c_str()));
  if (error.empty()) {
    FlushSystemDnsCache();  // after: the apps ask again of the box that runs now
  }
  return error;
}

std::string GoCore::Stop() {
  std::string error = TakeOwnedString(boxStop_());
  if (error.empty()) {
    FlushSystemDnsCache();  // what the box answered while it ran isn't true without it
  }
  return error;
}

CoreStats GoCore::Stats() {
  CoreStats stats;
  stats.running = boxStats_(&stats.uplinkBytes, &stats.downlinkBytes, &stats.activeConnections,
                            &stats.generation) != 0;
  return stats;
}

std::string GoCore::StartUrlTest(const UrlTestRequest& request) {
  nlohmann::json json;
  json["tags"] = request.tags;
  json["url"] = request.url;
  json["timeout_ms"] = request.timeout.count();
  return TakeOwnedString(boxUrlTest_(json.dump().c_str()));
}

std::vector<FlowTotal> GoCore::Flows() {
  // gocore/flows.go writes it: {"flows":{"<tag>":[up,down],...}}; anything
  // else in it is skipped, never thrown.
  const auto json = nlohmann::json::parse(TakeOwnedString(boxFlows_()), nullptr, /*allow_exceptions=*/false);
  std::vector<FlowTotal> flows;
  if (!json.is_object() || !json.contains("flows") || !json["flows"].is_object()) {
    return flows;
  }
  for (const auto& [tag, pair] : json["flows"].items()) {
    if (pair.is_array() && pair.size() == 2 && pair[0].is_number_integer() && pair[1].is_number_integer()) {
      flows.push_back({.tag = tag, .upBytes = pair[0].get<std::int64_t>(), .downBytes = pair[1].get<std::int64_t>()});
    }
  }
  return flows;
}

std::optional<RouteSeen> GoCore::RouteOf(const std::vector<std::string>& targets) {
  // gocore/route.go writes it: {"route":{"outbound","type","chain","rule"}},
  // or {} - none kept; a field of the wrong type is left empty.
  const auto json =
      nlohmann::json::parse(TakeOwnedString(boxRoute_(nlohmann::json(targets).dump().c_str())), nullptr,
                            /*allow_exceptions=*/false);
  if (!json.is_object() || !json.contains("route") || !json["route"].is_object()) {
    return std::nullopt;
  }
  const auto& route = json["route"];
  const auto text = [&](const char* key) {
    const auto it = route.find(key);
    return it != route.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  RouteSeen seen{.outbound = text("outbound"), .type = text("type"), .chain = {}, .rule = text("rule")};
  if (seen.outbound.empty()) {
    return std::nullopt;
  }
  if (const auto chain = route.find("chain"); chain != route.end() && chain->is_array()) {
    for (const auto& tag : *chain) {
      if (tag.is_string()) {
        seen.chain.push_back(tag.get<std::string>());
      }
    }
  }
  return seen;
}

std::vector<DelayResult> GoCore::Delays() {
  // gocore/urltest.go writes it; anything unexpected in it is skipped rather
  // than thrown - a broken answer must not take the control pipe down.
  const auto json = nlohmann::json::parse(TakeOwnedString(boxDelays_()), nullptr, /*allow_exceptions=*/false);
  std::vector<DelayResult> results;
  if (!json.is_object() || !json.contains("results") || !json["results"].is_array()) {
    return results;
  }
  for (const auto& item : json["results"]) {
    if (!item.is_object() || !item.contains("tag") || !item["tag"].is_string()) {
      continue;
    }
    // An optional count, clamped to what the Go side can send.
    const auto number = [&](const char* key, std::uint64_t most) {
      const auto it = item.find(key);
      return it != item.end() && it->is_number_unsigned()
                 ? static_cast<int>(std::min<std::uint64_t>(it->get<std::uint64_t>(), most))
                 : 0;
    };
    DelayResult result;
    result.tag = item["tag"].get<std::string>();
    result.connectMs = number("connect", 65535);
    if (item.contains("delay") && item["delay"].is_number_unsigned()) {
      result.state = DelayResult::State::Ok;
      result.delayMs = static_cast<int>(std::min<std::uint64_t>(item["delay"].get<std::uint64_t>(), 65535));
      result.jitterMs = number("jitter", 65535);
      result.lossPercent = number("loss", 100);
      result.samples = number("samples", 255);
    } else if (item.contains("error") && item["error"].is_string()) {
      result.state = DelayResult::State::Failed;
      result.error = item["error"].get<std::string>();
    }
    results.push_back(std::move(result));
  }
  return results;
}

std::string GoCore::Select(const std::string& selector, const std::string& outbound) {
  return TakeOwnedString(boxSelect_(selector.c_str(), outbound.c_str()));
}

ExitIp GoCore::LookupExitIp(const std::string& tag, bool refresh) {
  nlohmann::json request;
  request["tag"] = tag;
  request["refresh"] = refresh;
  // gocore/exitip.go writes it; a field of the wrong type is left empty.
  const auto json =
      nlohmann::json::parse(TakeOwnedString(boxExitIp_(request.dump().c_str())), nullptr, /*allow_exceptions=*/false);
  ExitIp result;
  result.tag = tag;
  if (!json.is_object()) {
    result.error = "unexpected answer from the core";
    return result;
  }
  const auto text = [&](const char* key) {
    const auto it = json.find(key);
    return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  result.ip = text("ip");
  result.country = text("country");
  result.isp = text("isp");
  result.error = text("error");
  const auto pending = json.find("pending");
  result.pending = pending != json.end() && pending->is_boolean() && pending->get<bool>();
  return result;
}

SiteResults GoCore::LookupSites(const std::string& tag, bool refresh) {
  SiteResults result;
  result.tag = tag;
  if (boxSites_ == nullptr) {
    result.error = "not supported by this core";
    return result;
  }
  nlohmann::json request;
  request["tag"] = tag;
  request["refresh"] = refresh;
  // gocore/sites.go writes it; whatever is of the wrong shape or type is left out.
  const auto json =
      nlohmann::json::parse(TakeOwnedString(boxSites_(request.dump().c_str())), nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    result.error = "unexpected answer from the core";
    return result;
  }
  const auto text = [](const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  result.error = text(json, "error");
  const auto pending = json.find("pending");
  result.pending = pending != json.end() && pending->is_boolean() && pending->get<bool>();
  const auto list = json.find("results");
  if (list != json.end() && list->is_array()) {
    for (const auto& item : *list) {
      if (!item.is_object()) {
        continue;
      }
      SiteResult site{.site = text(item, "site"), .state = text(item, "state"), .error = text(item, "error")};
      if (!site.site.empty()) {
        result.results.push_back(std::move(site));
      }
    }
  }
  return result;
}

}  // namespace sovereign::service
