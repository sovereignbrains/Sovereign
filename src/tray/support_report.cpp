#include "support_report.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <regex>
#include <string>
#include <utility>

namespace sovereign::tray {

namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
  return out;
}

bool HasAny(const std::string& text, std::initializer_list<std::string_view> needles) {
  return std::any_of(needles.begin(), needles.end(), [&](std::string_view n) { return text.find(n) != std::string::npos; });
}

std::string Str(const nlohmann::json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool Enabled(const nlohmann::json& object, const char* key) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_object()) {
    return false;
  }
  const auto on = it->find("enabled");
  return on != it->end() && on->is_boolean() && on->get<bool>();
}

// An IPv4 address that isn't anyone's on the internet: the PC's own, the
// local network's, the provider's NAT.
bool PrivateV4(std::string_view ip) {
  std::array<int, 4> part{};
  std::size_t at = 0;
  for (int& p : part) {
    const auto [end, ec] = std::from_chars(ip.data() + at, ip.data() + ip.size(), p);
    if (ec != std::errc{}) {
      return false;
    }
    at = static_cast<std::size_t>(end - ip.data()) + 1;
  }
  return part[0] == 10 || part[0] == 127 || (part[0] == 172 && part[1] >= 16 && part[1] <= 31) ||
         (part[0] == 192 && part[1] == 168) || (part[0] == 169 && part[1] == 254) ||
         (part[0] == 100 && part[1] >= 64 && part[1] <= 127);
}

bool PrivateV6(std::string_view ip) {
  const std::string lower = Lower(ip);
  return lower == "::1" || lower.starts_with("fe80") || lower.starts_with("fc") || lower.starts_with("fd");
}

// 1.5 ГБ, 300 МБ: bytes the way people count them (by 1024).
std::string Bytes(std::uint64_t n) {
  constexpr double kGb = 1024.0 * 1024 * 1024;
  constexpr double kMb = 1024.0 * 1024;
  std::string text = static_cast<double>(n) >= kGb ? std::format("{:.1f} ГБ", static_cast<double>(n) / kGb)
                              : std::format("{:.0f} МБ", static_cast<double>(n) / kMb);
  std::replace(text.begin(), text.end(), '.', ',');
  return text;
}

// "06.10.2026 14:20": unix seconds in the user's zone.
std::string When(std::int64_t unix, int offsetMinutes) {
  using namespace std::chrono;
  const sys_seconds t{seconds(unix + std::int64_t{offsetMinutes} * 60)};
  const auto day = floor<days>(t);
  const year_month_day ymd{day};
  const hh_mm_ss hms{t - day};
  return std::format("{:02}.{:02}.{} {:02}:{:02}", static_cast<unsigned>(ymd.day()), static_cast<unsigned>(ymd.month()),
                     static_cast<int>(ymd.year()), hms.hours().count(), hms.minutes().count());
}

}  // namespace

std::string FlagCountry(std::string_view name) {
  // A regional indicator is U+1F1E6..U+1F1FF: F0 9F 87 A6..BF in UTF-8; a
  // flag is two of them in a row.
  const auto letter = [&](std::size_t i) -> char {
    if (i + 3 < name.size() && static_cast<unsigned char>(name[i]) == 0xF0 &&
        static_cast<unsigned char>(name[i + 1]) == 0x9F && static_cast<unsigned char>(name[i + 2]) == 0x87) {
      const auto last = static_cast<unsigned char>(name[i + 3]);
      if (last >= 0xA6 && last <= 0xBF) {
        return static_cast<char>('A' + (last - 0xA6));
      }
    }
    return 0;
  };
  for (std::size_t i = 0; i + 7 < name.size(); ++i) {
    const char first = letter(i);
    const char second = first != 0 ? letter(i + 4) : '\0';
    if (second != 0) {
      return {first, second};
    }
  }
  return {};
}

std::string CutAddresses(std::string_view text, std::string_view userIp) {
  static const std::regex kV4(R"((\d{1,3}(?:\.\d{1,3}){3})(:\d+)?)");
  static const std::regex kV6(R"(\[([0-9A-Fa-f:]*:[0-9A-Fa-f:.]*)\](:\d+)?)");
  const std::string user(userIp);
  const auto cut = [&](const std::string& in, const std::regex& re, bool v6) {
    std::string out;
    auto from = in.cbegin();
    for (std::sregex_iterator it(in.cbegin(), in.cend(), re), end; it != end; ++it) {
      const std::string ip = (*it)[1].str();
      out.append(from, (*it)[0].first);
      const bool hide = (!user.empty() && Lower(ip) == Lower(user)) || (v6 ? PrivateV6(ip) : PrivateV4(ip));
      out += hide ? std::string("<...>") : (*it)[0].str();
      from = (*it)[0].second;
    }
    out.append(from, in.cend());
    return out;
  };
  std::string out = cut(std::string(text), kV4, false);
  out = cut(out, kV6, true);
  // A bare IPv6 (no brackets) only matters when it's the user's.
  if (!user.empty() && user.find(':') != std::string::npos) {
    for (std::size_t at = 0; (at = Lower(out).find(Lower(user), at)) != std::string::npos;) {
      out.replace(at, user.size(), "<...>");
    }
  }
  return out;
}

std::vector<ReportServer> ReportServers(std::string_view config) {
  std::vector<ReportServer> servers;
  const auto json = nlohmann::json::parse(config, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    return servers;
  }
  for (const char* list : {"outbounds", "endpoints"}) {
    const auto it = json.find(list);
    if (it == json.end() || !it->is_array()) {
      continue;
    }
    for (const nlohmann::json& o : *it) {
      if (!o.is_object()) {
        continue;
      }
      const std::string type = Str(o, "type");
      if (type.empty() || type == "selector" || type == "urltest" || type == "direct" || type == "block" ||
          type == "dns") {
        continue;
      }
      ReportServer s;
      s.name = Str(o, "tag");
      if (type == "wireguard") {
        const auto peers = o.find("peers");
        if (peers != o.end() && peers->is_array() && !peers->empty() && (*peers)[0].is_object()) {
          s.host = Str((*peers)[0], "address");
          s.port = (*peers)[0].value("port", 0);
        }
      } else {
        s.host = Str(o, "server");
        const auto port = o.find("server_port");
        s.port = port != o.end() && port->is_number_integer() ? port->get<int>() : 0;
      }
      std::string chain = type;
      if (const auto t = o.find("transport"); t != o.end() && t->is_object() && !Str(*t, "type").empty()) {
        chain += " · " + Str(*t, "type");
      }
      if (const auto tls = o.find("tls"); tls != o.end() && tls->is_object() && tls->value("enabled", false)) {
        chain += Enabled(*tls, "reality") ? " · REALITY" : Enabled(*tls, "ech") ? " · TLS · ECH" : " · TLS";
      }
      if (const auto obfs = o.find("obfs"); obfs != o.end() && obfs->is_object() && !Str(*obfs, "type").empty()) {
        chain += " · " + Str(*obfs, "type");
      }
      s.protocol = std::move(chain);
      servers.push_back(std::move(s));
    }
  }
  return servers;
}

std::string ServerProblem(const ReportServer& server) {
  if (server.state == ReportServer::State::Failed) {
    const std::string e = Lower(server.error);
    if (HasAny(e, {"unknown user", "authentication", "auth failed", "invalid user", "unauthorized", "reality verification",
                   "bad user", "password"})) {
      return "сервер отклоняет ключ подключения — подписка, вероятно, устарела (ключ на сервере сменили)";
    }
    if (HasAny(e, {"x509", "certificate"})) {
      return "сертификат сервера не прошёл проверку — истёк или выписан не на этот домен";
    }
    if (HasAny(e, {"no such host", "nxdomain", "server misbehaving"})) {
      return "имя сервера не находится в DNS — домен не настроен или удалён";
    }
    if (e.find("lookup ") != std::string::npos) {
      return "не удалось узнать адрес сервера — у пользователя в этот момент не было связи с DNS";
    }
    if (HasAny(e, {"connection refused", "actively refused"})) {
      return "сервер отвечает, но порт закрыт — сервис на сервере не запущен";
    }
    if (HasAny(e, {"timeout", "timed out", "deadline exceeded", "did not properly respond", "no route"})) {
      return "сервер не отвечает — выключен, или его адрес недоступен из сети пользователя";
    }
    if (HasAny(e, {"eof", "reset", "forcibly closed", "broken pipe", "closed"})) {
      return "соединение обрывается сразу после начала — сервер его сбрасывает, или его режет провайдер пользователя";
    }
    return "не подключается";
  }
  if (server.state == ReportServer::State::Ok) {
    if (server.loss >= 20) {
      return std::format("работает с большими потерями — {} % запросов без ответа", server.loss);
    }
    if (server.delay >= 1000) {
      return std::format("работает очень медленно — задержка {} мс", server.delay);
    }
    const std::string flag = FlagCountry(server.name);
    if (!flag.empty() && !server.exitCountry.empty() && Lower(flag) != Lower(server.exitCountry)) {
      return std::format("в названии страна {}, а выход на самом деле — {}", flag, server.exitCountry);
    }
  }
  return {};
}

SupportReport BuildSupportReport(const ReportInput& in) {
  std::size_t working = 0;
  std::size_t measured = 0;
  std::vector<std::pair<std::string, std::string>> problems;
  for (const ReportServer& s : in.servers) {
    measured += s.state != ReportServer::State::Unknown ? 1 : 0;
    working += s.state == ReportServer::State::Ok ? 1 : 0;
    if (std::string p = ServerProblem(s); !p.empty()) {
      problems.emplace_back(s.name, std::move(p));
    }
  }

  std::string verdict;
  if (!in.connected || measured == 0) {
    verdict = std::format("серверы не проверены — клиент не был подключён ({} в подписке)", in.servers.size());
  } else if (working == 0 && measured == in.servers.size()) {
    verdict = std::format("не работает ни один из {} серверов — возможно, у пользователя нет интернета, "
                          "или подписка отключена целиком",
                          in.servers.size());
  } else if (problems.empty()) {
    verdict = std::format("все {} серверов работают", in.servers.size());
  } else {
    verdict = std::format("работает {} из {} серверов", working, in.servers.size());
  }
  if (!in.refreshError.empty()) {
    verdict += "; подписка не обновляется";
  }
  if (in.expire > 0 && in.expire < in.now) {
    verdict += "; срок подписки истёк";
  }
  if (in.trafficTotal > 0 && in.trafficUsed >= in.trafficTotal) {
    verdict += "; трафик исчерпан";
  }

  // Only what's about their service: the dates in UTC (not the user's zone),
  // no client, no system, nothing of the user's network.
  const auto utc = [](std::int64_t unix) { return When(unix, 0) + " UTC"; };
  std::string full;
  full += std::format("Отчёт для поддержки подписки «{}»\n", in.subscriptionName);
  full += std::format("Составлен {}\n", utc(in.now));
  full += "Собран автоматически. Ключей, паролей, ссылки подписки и данных пользователя в нём нет.\n\n";

  full += "ИТОГ: " + verdict + "\n";
  for (const auto& [name, problem] : problems) {
    full += std::format("  • {} — {}\n", name, problem);
  }

  full += "\nПОДПИСКА\n";
  full += std::format("Адрес: {} (ссылка с токеном в отчёт не включена)\n",
                      in.subscriptionHost.empty() ? std::string("—") : in.subscriptionHost);
  full += "Обновлена: " + (in.lastRefresh > 0 ? utc(in.lastRefresh) : std::string("ни разу")) +
          (in.refreshError.empty() ? std::string() : " · последняя попытка не удалась: " +
                                                          CutAddresses(in.refreshError, in.userIp)) +
          "\n";
  if (in.trafficUsed > 0 || in.trafficTotal > 0) {
    full += "Трафик: " + Bytes(in.trafficUsed) +
            (in.trafficTotal > 0 ? " из " + Bytes(in.trafficTotal) : std::string(" (без лимита)")) + "\n";
  }
  if (in.expire > 0) {
    full += std::format("Оплачено до: {}{}\n", utc(in.expire), in.expire < in.now ? " — уже истекло" : "");
  }

  full += std::format("\nСЕРВЕРЫ ({})\n", in.servers.size());
  for (std::size_t i = 0; i < in.servers.size(); ++i) {
    const ReportServer& s = in.servers[i];
    full += std::format("{}. {} — {}", i + 1, s.name, s.protocol);
    if (!s.host.empty()) {
      full += " · " + (s.host.find(':') != std::string::npos ? "[" + s.host + "]" : s.host) +
              (s.port > 0 ? std::format(":{}", s.port) : std::string());
    }
    full += "\n";
    switch (s.state) {
      case ReportServer::State::Ok:
        full += std::format("   Работает: задержка {} мс, джиттер {} мс, потери {} %\n", s.delay, s.jitter, s.loss);
        break;
      case ReportServer::State::Failed:
        full += "   НЕ РАБОТАЕТ\n";
        if (!s.error.empty()) {
          full += "   Ошибка: " + CutAddresses(s.error, in.userIp) + "\n";
        }
        break;
      case ReportServer::State::Unknown: full += "   Не проверен\n"; break;
    }
    if (!s.exitIp.empty() || !s.exitCountry.empty()) {
      full += "   Выход в интернет: " + s.exitIp + (s.exitCountry.empty() ? "" : " · " + s.exitCountry) +
              (s.exitIsp.empty() ? "" : " · " + s.exitIsp) + "\n";
    }
    if (std::string p = ServerProblem(s); !p.empty()) {
      full += "   Проблема: " + p + "\n";
    }
  }

  std::string brief = std::format("Подписка «{}»: {}.\n", in.subscriptionName, verdict);
  for (std::size_t i = 0; i < problems.size() && i < 5; ++i) {
    brief += std::format("• {} — {}\n", problems[i].first, problems[i].second);
  }
  if (problems.size() > 5) {
    brief += std::format("• и ещё {}\n", problems.size() - 5);
  }
  brief += std::format("Подробный отчёт от {} — в файле.", utc(in.now));
  return {.full = std::move(full), .brief = std::move(brief)};
}

std::string SubscriptionTrouble(std::size_t servers, std::size_t measured, std::size_t failed, bool refreshFailed,
                                std::uint64_t used, std::uint64_t total, std::int64_t expire, std::int64_t now) {
  if (expire > 0 && expire < now) {
    return "оплата истекла";
  }
  if (total > 0 && used >= total) {
    return "трафик закончился";
  }
  if (measured > 0 && failed == measured) {
    return measured == 1 ? "сервер не отвечает" : "не отвечает ни один сервер";
  }
  if (failed > 0 && failed * 2 >= measured) {
    return std::format("не работает {} из {} серверов", failed, std::max(servers, measured));
  }
  if (refreshFailed) {
    return "подписка не обновляется";
  }
  return {};
}

std::string UsageLine(std::uint64_t used, std::uint64_t total, std::int64_t expire, std::int64_t now,
                      int utcOffsetMinutes) {
  std::string line;
  if (used > 0 || total > 0) {
    line = "Трафик: " + Bytes(used) + (total > 0 ? " из " + Bytes(total) : std::string(" (без лимита)"));
  }
  if (expire > 0) {
    const std::string day = When(expire, utcOffsetMinutes).substr(0, 10);
    line += (line.empty() ? std::string() : std::string(" · ")) +
            (expire < now ? "оплата истекла " + day : "оплачено до " + day);
  }
  return line;
}

}  // namespace sovereign::tray
