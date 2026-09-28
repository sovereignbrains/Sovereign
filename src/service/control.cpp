#include "control.h"

#include <nlohmann/json.hpp>

#include "sha256.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace sovereign::service {

namespace {

using Json = nlohmann::json;

Json Error(const std::string& message) {
  Json response;
  response["cmd"] = "error";
  response["message"] = message;
  return response;
}

// Core log lines are text from the network side (domains, peer errors) and
// not guaranteed to be valid UTF-8; nlohmann's default dump throws on that.
std::string Dump(const Json& json) {
  return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json StatsResponse(const CoreStats& stats) {
  Json response;
  response["cmd"] = "box_stats";
  response["running"] = stats.running;
  response["uplink"] = stats.uplinkBytes;
  response["downlink"] = stats.downlinkBytes;
  response["connections"] = stats.activeConnections;
  response["generation"] = stats.generation;
  return response;
}

Json LogsResponse(const LogRing& log, std::uint64_t since) {
  // A reader ahead of the newest line kept its place from before a service
  // restart (numbering starts over with the service): read from the start,
  // or it would wait for the new numbers to catch up with the old ones.
  if (since > log.LastSeq()) {
    since = 0;
  }
  Json entries = Json::array();
  std::uint64_t next = since;
  bool more = false;
  // Rough fixed cost of the envelope around the entries array.
  std::size_t size = 64;
  for (const LogEntry& entry : log.Since(since)) {
    Json item;
    item["seq"] = entry.seq;
    item["level"] = std::string(LogLevelName(entry.level));
    item["message"] = entry.message;
    item["time"] = entry.timeMs;
    const std::size_t itemSize = Dump(item).size() + 1;
    if (size + itemSize > ControlHandler::kMaxLogsResponseBytes) {
      more = true;
      break;
    }
    size += itemSize;
    entries.push_back(std::move(item));
    next = entry.seq;
  }
  // Nothing new: point `next` at the newest line so a reader that started
  // behind a ring drop does not keep asking for lines that are gone.
  if (entries.empty() && !more) {
    next = std::max(since, log.LastSeq());
  }
  Json response;
  response["cmd"] = "box_logs";
  response["entries"] = std::move(entries);
  response["next"] = next;
  response["more"] = more;
  return response;
}

// A URL for box_urltest: http(s), with a host, nothing that could split a
// request line - the core dials it as given.
bool IsTestUrl(const Json& url) {
  if (!url.is_string()) {
    return false;
  }
  const std::string& text = url.get_ref<const std::string&>();
  if (text.size() > ControlHandler::kMaxUrlBytes) {
    return false;
  }
  const std::string_view view(text);
  std::size_t host = 0;
  if (view.starts_with("https://")) {
    host = 8;
  } else if (view.starts_with("http://")) {
    host = 7;
  } else {
    return false;
  }
  return view.size() > host && view[host] != '/' &&
         std::none_of(view.begin(), view.end(), [](char c) { return static_cast<unsigned char>(c) <= ' ' || c == 0x7F; });
}

}  // namespace

ControlHandler::ControlHandler(ICore* core, const LogRing& coreLog,
                               std::optional<std::string>& lastConfig, CommandObserver observer,
                               IKillSwitch* killSwitch)
    : core_(core),
      coreLog_(coreLog),
      lastConfig_(lastConfig),
      observer_(std::move(observer)),
      killSwitch_(killSwitch),
      killActive_(killSwitch != nullptr && killSwitch->Active()) {
  killSettings_.enabled = killActive_;
}

std::string ControlHandler::ApplyKillSwitch(const KillSwitchSettings& settings) {
  const std::vector<Prefix> tun = lastConfig_ ? TunPrefixes(*lastConfig_) : std::vector<Prefix>{};
  std::string error = killSwitch_->Apply(BuildKillSwitchRules(settings, tun));
  if (error.empty()) {
    killSettings_ = settings;
    killActive_ = settings.enabled;
  }
  return error;
}

std::string ControlHandler::Handle(const std::string& request) {
  const auto started = std::chrono::steady_clock::now();
  Outcome outcome;
  std::string response = Dispatch(request, outcome);
  if (observer_) {
    observer_(CommandRecord{
        .cmd = outcome.cmd,
        .requestBytes = request.size(),
        .error = outcome.error,
        .duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started),
    });
  }
  return response;
}

std::string ControlHandler::Dispatch(const std::string& request, Outcome& outcome) {
  const auto fail = [&outcome](std::string message) {
    outcome.error = std::move(message);
    return Dump(Error(outcome.error));
  };

  Json parsed;
  try {
    parsed = Json::parse(request);
  } catch (const Json::exception&) {
    // Not only parse_error: a number too big for a double ("1e999") is an
    // out_of_range - found by tests/fuzz/control_fuzz.cpp, the client used to
    // get no answer at all.
    return fail("invalid json");
  }
  if (!parsed.is_object()) {
    return fail("request must be a json object");
  }

  // A cmd that isn't a string is a malformed request. (value() would throw on
  // it - the pipe server caught that, but the client got no answer at all;
  // found by tests/fuzz/control_fuzz.cpp.)
  if (parsed.contains("cmd") && !parsed.at("cmd").is_string()) {
    return fail("cmd must be a string");
  }
  // outcome.cmd only ever points at these literals, never at request text.
  const std::string cmd = parsed.value("cmd", "");
  for (const std::string_view known :
       {"box_ping", "box_start", "box_stop", "box_stats", "box_logs", "box_urltest", "box_delays", "box_exitip",
        "kill_switch"}) {
    if (cmd == known) {
      outcome.cmd = known;
    }
  }
  if (outcome.cmd == "invalid") {
    outcome.cmd = "echo";
  }

  const bool isCoreCommand = cmd == "box_ping" || cmd == "box_start" || cmd == "box_stop" ||
                             cmd == "box_stats" || cmd == "box_urltest" || cmd == "box_delays" ||
                             cmd == "box_exitip";
  if (isCoreCommand && core_ == nullptr) {
    return fail("gocore not loaded");
  }

  if (cmd == "box_ping") {
    Json response;
    response["cmd"] = "box_pong";
    response["reply"] = core_->Ping();
    return Dump(response);
  }

  if (cmd == "box_start") {
    if (!parsed.contains("config")) {
      return fail("missing config field");
    }
    // "config" carries a full sing-box config document as a nested JSON
    // object (not a pre-serialized string) — natural for a JSON-over-pipe
    // request. It is re-serialized here because the core takes the config as
    // raw text, same as `sing-box run -c`.
    const std::string configJson = parsed.at("config").dump();
    const std::string error = core_->Start(configJson);
    if (!error.empty()) {
      return fail(error);
    }
    lastConfig_ = configJson;
    Json response;
    response["cmd"] = "box_started";
    // A new config may bring a tunnel of other addresses: the kill switch
    // follows. Its failure doesn't undo the start - it's reported with it.
    if (killSwitch_ != nullptr && killSettings_.enabled) {
      if (const std::string error = ApplyKillSwitch(killSettings_); !error.empty()) {
        response["kill_switch_error"] = error;
      }
    }
    return Dump(response);
  }

  if (cmd == "box_stop") {
    const std::string error = core_->Stop();
    if (!error.empty()) {
      return fail(error);
    }
    lastConfig_.reset();
    Json response;
    response["cmd"] = "box_stopped";
    return Dump(response);
  }

  if (cmd == "box_stats") {
    Json response = StatsResponse(core_->Stats());
    // Which config the box runs, without the config itself (it holds keys):
    // the tray compares it with its own config.json and restarts the box on a
    // mismatch - e.g. config.json changed while the tray wasn't running.
    if (lastConfig_) {
      response["config_sha256"] = Sha256Hex(*lastConfig_);
    }
    response["kill_switch"] = killActive_;
    response["kill_switch_lan"] = killSettings_.allowLan;
    return Dump(response);
  }

  if (cmd == "kill_switch") {
    if (killSwitch_ == nullptr) {
      return fail("kill switch unavailable");
    }
    const Json enabled = parsed.value("enabled", Json());
    const Json allowLan = parsed.value("allow_lan", Json(true));
    if (!enabled.is_boolean() || !allowLan.is_boolean()) {
      return fail("enabled and allow_lan must be booleans");
    }
    const KillSwitchSettings settings{.enabled = enabled.get<bool>(), .allowLan = allowLan.get<bool>()};
    if (settings != killSettings_ || settings.enabled != killActive_) {
      if (const std::string error = ApplyKillSwitch(settings); !error.empty()) {
        return fail(error);
      }
    }
    Json response;
    response["cmd"] = "kill_switch";
    response["active"] = killActive_;
    response["allow_lan"] = killSettings_.allowLan;
    return Dump(response);
  }

  if (cmd == "box_logs") {
    // The log ring is the service's, not the core's: it keeps answering
    // (with whatever was logged) even when no core is loaded.
    const Json since = parsed.value("since", Json(0));
    if (!since.is_number_unsigned() && !(since.is_number_integer() && since.get<std::int64_t>() >= 0)) {
      return fail("since must be a non-negative integer");
    }
    return Dump(LogsResponse(coreLog_, since.get<std::uint64_t>()));
  }

  if (cmd == "box_urltest") {
    UrlTestRequest test;
    const Json tags = parsed.value("tags", Json());
    if (!tags.is_array() || tags.empty() || tags.size() > kMaxUrlTestTags) {
      return fail("tags must be an array of 1.." + std::to_string(kMaxUrlTestTags) + " outbound tags");
    }
    for (const Json& tag : tags) {
      if (!tag.is_string() || tag.get<std::string>().empty() || tag.get<std::string>().size() > kMaxTagBytes) {
        return fail("every tag must be a non-empty string");
      }
      test.tags.push_back(tag.get<std::string>());
    }
    const Json url = parsed.value("url", Json(std::string(kDefaultUrlTestUrl)));
    if (!IsTestUrl(url)) {
      return fail("url must be an http(s) URL");
    }
    test.url = url.get<std::string>();
    const Json timeout = parsed.value("timeout_ms", Json(kDefaultUrlTestTimeoutMs));
    if (!timeout.is_number_integer() || timeout.get<std::int64_t>() < kMinUrlTestTimeoutMs ||
        timeout.get<std::int64_t>() > kMaxUrlTestTimeoutMs) {
      return fail("timeout_ms must be " + std::to_string(kMinUrlTestTimeoutMs) + ".." +
                  std::to_string(kMaxUrlTestTimeoutMs));
    }
    test.timeout = std::chrono::milliseconds(timeout.get<std::int64_t>());
    const std::string error = core_->StartUrlTest(test);
    if (!error.empty()) {
      return fail(error);
    }
    Json response;
    response["cmd"] = "box_urltest_started";
    return Dump(response);
  }

  if (cmd == "box_delays") {
    Json results = Json::array();
    for (const DelayResult& result : core_->Delays()) {
      Json item;
      item["tag"] = result.tag;
      switch (result.state) {
        case DelayResult::State::Pending: item["pending"] = true; break;
        case DelayResult::State::Ok: item["delay"] = result.delayMs; break;
        case DelayResult::State::Failed: item["error"] = result.error; break;
      }
      results.push_back(std::move(item));
    }
    Json response;
    response["cmd"] = "box_delays";
    response["results"] = std::move(results);
    return Dump(response);
  }

  if (cmd == "box_exitip") {
    const Json tag = parsed.value("tag", Json());
    if (!tag.is_string() || tag.get<std::string>().empty() || tag.get<std::string>().size() > kMaxTagBytes) {
      return fail("tag must be a non-empty string");
    }
    const Json refresh = parsed.value("refresh", Json(false));
    if (!refresh.is_boolean()) {
      return fail("refresh must be a boolean");
    }
    const ExitIp exit = core_->LookupExitIp(tag.get<std::string>(), refresh.get<bool>());
    Json response;
    response["cmd"] = "box_exitip";
    response["tag"] = exit.tag;
    if (exit.pending) {
      response["pending"] = true;
    }
    if (!exit.ip.empty()) {
      response["ip"] = exit.ip;
    }
    if (!exit.country.empty()) {
      response["country"] = exit.country;
    }
    if (!exit.error.empty()) {
      response["error"] = exit.error;
    }
    return Dump(response);
  }

  // Anything else echoes back (the P0 contract the smoke tests still use).
  Json response;
  response["cmd"] = "pong";
  response["echo"] = parsed;
  return Dump(response);
}

}  // namespace sovereign::service
