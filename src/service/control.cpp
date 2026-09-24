#include "control.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
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

}  // namespace

ControlHandler::ControlHandler(ICore* core, const LogRing& coreLog,
                               std::optional<std::string>& lastConfig)
    : core_(core), coreLog_(coreLog), lastConfig_(lastConfig) {}

std::string ControlHandler::Handle(const std::string& request) {
  Json parsed;
  try {
    parsed = Json::parse(request);
  } catch (const Json::parse_error&) {
    return Dump(Error("invalid json"));
  }
  if (!parsed.is_object()) {
    return Dump(Error("request must be a json object"));
  }

  const std::string cmd = parsed.value("cmd", "");
  const bool isCoreCommand = cmd == "box_ping" || cmd == "box_start" || cmd == "box_stop" ||
                             cmd == "box_stats";
  if (isCoreCommand && core_ == nullptr) {
    return Dump(Error("gocore not loaded"));
  }

  if (cmd == "box_ping") {
    Json response;
    response["cmd"] = "box_pong";
    response["reply"] = core_->Ping();
    return Dump(response);
  }

  if (cmd == "box_start") {
    if (!parsed.contains("config")) {
      return Dump(Error("missing config field"));
    }
    // "config" carries a full sing-box config document as a nested JSON
    // object (not a pre-serialized string) — natural for a JSON-over-pipe
    // request. It is re-serialized here because the core takes the config as
    // raw text, same as `sing-box run -c`.
    const std::string configJson = parsed.at("config").dump();
    const std::string error = core_->Start(configJson);
    if (!error.empty()) {
      return Dump(Error(error));
    }
    lastConfig_ = configJson;
    Json response;
    response["cmd"] = "box_started";
    return Dump(response);
  }

  if (cmd == "box_stop") {
    const std::string error = core_->Stop();
    if (!error.empty()) {
      return Dump(Error(error));
    }
    lastConfig_.reset();
    Json response;
    response["cmd"] = "box_stopped";
    return Dump(response);
  }

  if (cmd == "box_stats") {
    return Dump(StatsResponse(core_->Stats()));
  }

  if (cmd == "box_logs") {
    // The log ring is the service's, not the core's: it keeps answering
    // (with whatever was logged) even when no core is loaded.
    const Json since = parsed.value("since", Json(0));
    if (!since.is_number_unsigned() && !(since.is_number_integer() && since.get<std::int64_t>() >= 0)) {
      return Dump(Error("since must be a non-negative integer"));
    }
    return Dump(LogsResponse(coreLog_, since.get<std::uint64_t>()));
  }

  // Anything else echoes back (the P0 contract the smoke tests still use).
  Json response;
  response["cmd"] = "pong";
  response["echo"] = parsed;
  return Dump(response);
}

}  // namespace sovereign::service
