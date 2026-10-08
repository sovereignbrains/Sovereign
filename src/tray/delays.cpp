#include "delays.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

#include "json_field.h"

namespace sovereign::tray {

std::optional<std::map<std::string, Delay>> ParseDelaysResponse(std::string_view response) {
  const auto json = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);
  if (Field<std::string>(json, "cmd", {}) != "box_delays" || !json.contains("results") ||
      !json["results"].is_array()) {
    return std::nullopt;
  }
  std::map<std::string, Delay> delays;
  for (const auto& item : json["results"]) {
    const std::string tag = Field<std::string>(item, "tag", {});
    if (tag.empty()) {
      continue;
    }
    // A count the service reports, clamped; anything else is 0.
    const auto count = [&](const char* key, std::int64_t most) {
      return static_cast<int>(std::clamp<std::int64_t>(Field<std::int64_t>(item, key, 0), 0, most));
    };
    Delay delay;
    delay.connect = count("connect", 65535);
    const std::int64_t ms = Field<std::int64_t>(item, "delay", 0);
    const std::string error = Field<std::string>(item, "error", {});
    if (ms > 0) {
      delay.state = Delay::State::Ok;
      delay.ms = static_cast<int>(std::min<std::int64_t>(ms, 65535));
      delay.jitter = count("jitter", 65535);
      delay.loss = count("loss", 100);
      delay.samples = count("samples", 255);
    } else if (!error.empty()) {
      delay.state = Delay::State::Failed;
      delay.error = error;
    } else if (!Field<bool>(item, "pending", false)) {
      continue;  // not a result
    }
    delays[tag] = std::move(delay);
  }
  return delays;
}

int DelayScore(const Delay& delay) { return delay.ms + 2 * delay.jitter + 10 * delay.loss; }

AutoPick JudgeAuto(AutoPick pick, const std::vector<std::string>& members,
                   const std::map<std::string, Delay>& delays) {
  const auto measured = [&](const std::string& tag) -> const Delay* {
    const auto it = delays.find(tag);
    return it != delays.end() && it->second.state == Delay::State::Ok ? &it->second : nullptr;
  };
  const std::string* best = nullptr;
  int bestScore = 0;
  for (const std::string& tag : members) {
    if (const Delay* delay = measured(tag); delay != nullptr && (best == nullptr || DelayScore(*delay) < bestScore)) {
      best = &tag;
      bestScore = DelayScore(*delay);
    }
  }
  if (best == nullptr) {
    return pick;  // nothing answered: no better idea than the one there is
  }
  const bool member = std::find(members.begin(), members.end(), pick.server) != members.end();
  const auto current = delays.find(pick.server);
  const bool failed = current != delays.end() && current->second.state == Delay::State::Failed;
  if (pick.server.empty() || !member || failed) {
    return {.server = *best};
  }
  const Delay* now = measured(pick.server);
  if (now == nullptr || *best == pick.server) {
    return {.server = pick.server};  // not measured this time, or still the best
  }
  const int nowScore = DelayScore(*now);
  if (bestScore * 5 > nowScore * 4 || nowScore - bestScore < 20) {
    return {.server = pick.server};
  }
  pick.wins = *best == pick.candidate ? pick.wins + 1 : 1;
  pick.candidate = *best;
  return pick.wins >= 2 ? AutoPick{.server = *best} : pick;
}

std::string NextWarpServer(const std::vector<std::string>& servers, const std::vector<std::string>& tried,
                           const std::map<std::string, Delay>& delays) {
  // Lower is sooner: answered by its score, then not measured, failed last.
  const auto rank = [&](const std::string& tag) {
    const auto it = delays.find(tag);
    if (it == delays.end() || it->second.state == Delay::State::Pending) {
      return std::pair{1, 0};
    }
    return it->second.state == Delay::State::Ok ? std::pair{0, DelayScore(it->second)} : std::pair{2, 0};
  };
  const std::string* next = nullptr;
  for (const std::string& tag : servers) {
    if (std::find(tried.begin(), tried.end(), tag) == tried.end() && (next == nullptr || rank(tag) < rank(*next))) {
      next = &tag;
    }
  }
  return next != nullptr ? *next : std::string();
}

}  // namespace sovereign::tray
