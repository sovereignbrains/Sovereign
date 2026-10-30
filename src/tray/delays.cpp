#include "delays.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
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

std::optional<SiteChecks> ParseSitesResponse(std::string_view response) {
  const auto json = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);
  if (Field<std::string>(json, "cmd", {}) != "box_sites") {
    return std::nullopt;
  }
  SiteChecks checks;
  checks.pending = Field<bool>(json, "pending", false);
  checks.error = Field<std::string>(json, "error", {});
  if (checks.error.size() > 200) {
    checks.error.resize(200);
  }
  if (!json.contains("results") || !json["results"].is_array()) {
    return checks;
  }
  for (const auto& item : json["results"]) {
    std::string site = Field<std::string>(item, "site", {});
    // An id, not text: lowercase letters and digits, short.
    const bool plain = !site.empty() && site.size() <= 16 &&
                       std::all_of(site.begin(), site.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
    if (!plain) {
      continue;
    }
    const std::string state = Field<std::string>(item, "state", {});
    checks.sites.push_back({std::move(site), state == "ok"        ? SiteCheck::State::Ok
                                             : state == "blocked" ? SiteCheck::State::Blocked
                                                                  : SiteCheck::State::Fail});
  }
  return checks;
}

int DelayScore(const Delay& delay) { return delay.ms + 2 * delay.jitter + 10 * delay.loss; }

std::string BestServer(const std::vector<std::string>& members, const std::map<std::string, Delay>& delays) {
  const std::string* best = nullptr;
  int bestScore = 0;
  for (const std::string& tag : members) {
    const auto it = delays.find(tag);
    if (it == delays.end() || it->second.state != Delay::State::Ok) {
      continue;
    }
    if (best == nullptr || DelayScore(it->second) < bestScore) {
      best = &tag;
      bestScore = DelayScore(it->second);
    }
  }
  return best != nullptr ? *best : std::string();
}

AutoPick JudgeAuto(AutoPick pick, const std::vector<std::string>& members,
                   const std::map<std::string, Delay>& delays, const std::map<std::string, std::string>& hosts) {
  const auto measured = [&](const std::string& tag) -> const Delay* {
    const auto it = delays.find(tag);
    return it != delays.end() && it->second.state == Delay::State::Ok ? &it->second : nullptr;
  };
  const auto hostOf = [&](const std::string& tag) {
    const auto it = hosts.find(tag);
    return it != hosts.end() && !it->second.empty() ? it->second : tag;
  };
  const std::string best = BestServer(members, delays);
  if (best.empty()) {
    return pick;  // nothing answered: no better idea than the one there is
  }
  const bool member = std::find(members.begin(), members.end(), pick.server) != members.end();
  const auto current = delays.find(pick.server);
  const bool failed = current != delays.end() && current->second.state == Delay::State::Failed;
  if (pick.server.empty() || !member || failed) {
    return {.server = best};
  }
  const Delay* now = measured(pick.server);
  if (now == nullptr) {
    return {.server = pick.server};  // not measured this time
  }
  // The server in use is a host and the protocol on it: the host is worth its
  // best protocol, and that one is what the protocol is picked from.
  const std::string host = hostOf(pick.server);
  std::vector<std::string> onHost;
  std::copy_if(members.begin(), members.end(), std::back_inserter(onHost),
               [&](const std::string& tag) { return hostOf(tag) == host; });
  const std::string hostBest = BestServer(onHost, delays);
  const int nowScore = DelayScore(*now);
  const int hostScore = DelayScore(*measured(hostBest));

  // Another host: only for a clear win, twice in a row.
  AutoPick next{.server = pick.server};
  if (hostOf(best) != host) {
    const int bestScore = DelayScore(*measured(best));
    if (bestScore * 5 <= hostScore * 4 && hostScore - bestScore >= 20) {
      next.wins = !pick.candidate.empty() && hostOf(best) == hostOf(pick.candidate) ? pick.wins + 1 : 1;
      next.candidate = best;
      if (next.wins >= 2) {
        return {.server = best};
      }
    }
  }
  // The protocol on this host: the best one, once it's ten milliseconds better.
  if (hostBest != pick.server && nowScore - hostScore >= 10) {
    next.server = hostBest;
  }
  return next;
}

LocalNetwork JudgeLocalNetwork(const Delay& direct, int failedInRow, std::optional<int> bestMs, bool serversDown) {
  LocalNetwork n;
  if (direct.state == Delay::State::Failed && failedInRow >= 1 && serversDown) {
    n.state = LocalNetwork::State::Down;
    n.forUser = "Нет интернета даже без VPN — проверь Wi-Fi, кабель или провайдера.";
    n.forSupport = "у пользователя нет интернета даже без VPN";
    return n;
  }
  if (direct.state != Delay::State::Ok) {
    return n;
  }
  if (direct.loss >= 30) {
    n.state = LocalNetwork::State::Lossy;
    n.forUser = std::format("Твоя сеть теряет пакеты и без VPN: напрямую не доходит {}% запросов. "
                            "Проверь Wi-Fi или провайдера.",
                            direct.loss);
    n.forSupport = std::format("у пользователя теряется {}% запросов и без VPN", direct.loss);
    return n;
  }
  const bool slow = direct.ms >= 100 && (!bestMs || (direct.ms * 2 >= *bestMs * 5 && direct.ms - *bestMs >= 60));
  if (slow) {
    n.state = LocalNetwork::State::Slow;
    const std::string usual = bestMs && *bestMs < direct.ms ? std::format(" (обычно {})", *bestMs) : std::string();
    n.forUser = std::format("Тормозит твоя сеть, а не VPN: напрямую до интернета {} мс{}. "
                            "Проверь Wi-Fi или провайдера.",
                            direct.ms, usual);
    n.forSupport = std::format("у пользователя медленная сеть и без VPN: напрямую {} мс", direct.ms);
  }
  return n;
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
