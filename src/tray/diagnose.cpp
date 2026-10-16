#include "diagnose.h"

// windows.h's min/max macros break std::min/max (clang-tidy in CI doesn't see
// the global NOMINMAX for every file).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <windns.h>

#include <wil/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <random>
#include <string_view>
#include <thread>

#include "fetch.h"

namespace sovereign::tray {

namespace {

using Clock = std::chrono::steady_clock;
using Status = CheckResult::Status;

double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
int Millis(Clock::duration d) { return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()); }

std::wstring Widen(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
  return out;
}

std::string RandomLabel(std::size_t length) {
  static constexpr std::string_view kChars = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::random_device device;
  std::uniform_int_distribution<std::size_t> pick(0, kChars.size() - 1);
  std::string label;
  for (std::size_t i = 0; i < length; ++i) {
    label.push_back(kChars[pick(device)]);
  }
  return label;
}

// A name looked up the way programs do (the system's resolver, its cache
// skipped): through the TUN, the core's DNS rules. The time it took, or
// nullopt if it didn't resolve.
std::optional<Clock::duration> Resolve(const std::string& name) {
  const auto started = Clock::now();
  PDNS_RECORD records = nullptr;
  const DNS_STATUS status =
      DnsQuery_W(Widen(name).c_str(), DNS_TYPE_A, DNS_QUERY_BYPASS_CACHE, nullptr, &records, nullptr);
  const auto took = Clock::now() - started;
  if (records != nullptr) {
    DnsRecordListFree(records, DnsFreeRecordList);
  }
  // A name that doesn't exist answered quickly still measures the path.
  if (status != 0 && status != DNS_ERROR_RCODE_NAME_ERROR && status != DNS_INFO_NO_RECORDS) {
    return std::nullopt;
  }
  return took;
}

// The addresses `host` resolves to (getaddrinfo, as programs do).
std::vector<std::string> Addresses(const std::string& host) {
  std::vector<std::string> out;
  addrinfoW hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfoW* found = nullptr;
  if (GetAddrInfoW(Widen(host).c_str(), L"443", &hints, &found) != 0) {
    return out;
  }
  for (addrinfoW* a = found; a != nullptr; a = a->ai_next) {
    std::array<char, INET6_ADDRSTRLEN> text{};
    const void* address = a->ai_family == AF_INET
                              ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr)
                              : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(a->ai_addr)->sin6_addr);
    if (inet_ntop(a->ai_family, address, text.data(), text.size()) != nullptr) {
      out.emplace_back(text.data());
    }
  }
  FreeAddrInfoW(found);
  return out;
}

// What a STUN server sees of us - a browser's WebRTC would tell a site the
// same. nullopt: no answer (UDP doesn't get through).
std::optional<std::string> StunMappedAddress() {
  static constexpr std::array<std::pair<const wchar_t*, const wchar_t*>, 2> kServers = {
      {{L"stun.l.google.com", L"19302"}, {L"stun.cloudflare.com", L"3478"}}};
  for (const auto& [host, port] : kServers) {
    addrinfoW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfoW* found = nullptr;
    if (GetAddrInfoW(host, port, &hints, &found) != 0 || found == nullptr) {
      continue;
    }
    wil::unique_socket sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    DWORD timeout = 2500;
    setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    StunId id{};
    std::random_device device;
    for (auto& b : id) {
      b = static_cast<std::uint8_t>(device() & 0xFF);
    }
    const std::string request = StunRequest(id);
    std::optional<std::string> mapped;
    for (int attempt = 0; attempt < 2 && !mapped; ++attempt) {
      sendto(sock.get(), request.data(), static_cast<int>(request.size()), 0, found->ai_addr,
             static_cast<int>(found->ai_addrlen));
      std::array<char, 512> buffer{};
      const int got = recvfrom(sock.get(), buffer.data(), static_cast<int>(buffer.size()), 0, nullptr, nullptr);
      if (got > 0) {
        mapped = ParseStunResponse(std::string_view(buffer.data(), static_cast<std::size_t>(got)), id);
      }
    }
    FreeAddrInfoW(found);
    if (mapped) {
      return mapped;
    }
  }
  return std::nullopt;
}

// The router: the IPv4 gateway of a real adapter (not a TUN), and how fast
// it answers a ping.
std::pair<std::string, std::optional<int>> PingGateway() {
  ULONG size = 16 * 1024;
  std::vector<std::byte> buffer(size);
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                           reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size) == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                             reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size) != NO_ERROR) {
      return {};
    }
  }
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a != nullptr; a = a->Next) {
    const std::wstring_view description = a->Description != nullptr ? a->Description : L"";
    if (a->OperStatus != IfOperStatusUp || a->FirstGatewayAddress == nullptr ||
        description.find(L"sing-tun") != std::wstring_view::npos ||
        description.find(L"Wintun") != std::wstring_view::npos) {
      continue;
    }
    const auto* gateway = reinterpret_cast<sockaddr_in*>(a->FirstGatewayAddress->Address.lpSockaddr);
    std::array<char, INET_ADDRSTRLEN> text{};
    inet_ntop(AF_INET, &gateway->sin_addr, text.data(), text.size());
    const HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) {
      return {text.data(), std::nullopt};
    }
    std::array<char, 32> data{};
    std::array<std::byte, sizeof(ICMP_ECHO_REPLY) + 32 + 8> reply{};
    std::optional<int> rtt;
    for (int attempt = 0; attempt < 3 && !rtt; ++attempt) {
      if (IcmpSendEcho(icmp, gateway->sin_addr.S_un.S_addr, data.data(), static_cast<WORD>(data.size()), nullptr,
                       reply.data(), static_cast<DWORD>(reply.size()), 1000) > 0) {
        const auto* echo = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.data());
        if (echo->Status == IP_SUCCESS) {
          rtt = static_cast<int>(echo->RoundTripTime);
        }
      }
    }
    IcmpCloseHandle(icmp);
    return {text.data(), rtt};
  }
  return {};
}

// The speed test's samples, one a slot of kSpeedSampleMs from its start: the
// throughput since the last one, spread over every slot that passed - a stall
// (no bytes for a second) fills its slots with what came over it, so the n-th
// sample is always the n-th quarter of a second (taken as bytes came, a stall
// left none and the graph ended early - 16.10.2026).
struct Slots {
  Clock::time_point started;
  Clock::time_point sampledAt;
  std::uint64_t sampledBytes = 0;

  // `total` moved by `now`: true if a sample was added.
  bool Fill(std::vector<float>& samples, std::uint64_t total, Clock::time_point now) {
    const auto due = static_cast<std::size_t>((now - started) / std::chrono::milliseconds(kSpeedSampleMs));
    if (due <= samples.size()) {
      return false;
    }
    samples.resize(due, static_cast<float>(Mbps(total - sampledBytes, Seconds(now - sampledAt))));
    sampledAt = now;
    sampledBytes = total;
    return true;
  }
};

struct Run {
  const DiagnoseInput& input;
  std::string exitIp;   // what foreign sites see
  std::string exitLoc;
  std::string directIp;  // what Russian ones see
  // A check's result while it runs (the speed test's graph growing); may be empty.
  std::function<void(const CheckResult&)> live;

  // The VPN's exit: the core's, else - everything through the VPN - what the
  // trace saw; with only chosen things through it, unknown without the core.
  std::string VpnIp() const { return !input.vpnIp.empty() ? input.vpnIp : input.selective ? std::string() : exitIp; }
  std::string VpnLoc() const {
    return !input.vpnCountry.empty() ? input.vpnCountry : input.selective ? std::string() : exitLoc;
  }

  std::optional<Trace> FetchTrace(const wchar_t* url) const {
    const auto body = Download(url, input.userAgent, std::size_t{64} * 1024);
    return body ? ParseTrace(*body) : std::nullopt;
  }

  CheckResult Exit() {
    CheckResult r;
    const auto trace = FetchTrace(L"https://www.cloudflare.com/cdn-cgi/trace");
    if (!trace) {
      r.status = Status::Fail;
      r.summary = "нет ответа";
      r.detail = "Зарубежный сайт недоступен: подключение не работает или сервер не отвечает.";
      r.fix = CheckFix::Reconnect;
      return r;
    }
    exitIp = trace->ip;
    exitLoc = trace->loc;
    const std::string seen = trace->loc.empty() ? trace->ip : std::format("{} · {}", trace->ip, trace->loc);
    if (input.selective) {
      // The trace went directly, as anything unlisted: the VPN's exit is the core's.
      if (input.vpnIp.empty()) {
        r.status = Status::Warn;
        r.summary = "VPN ещё не ответил";
        r.detail = "Адрес VPN пока не известен — проверь снова через пару секунд после подключения.";
        return r;
      }
      r.status = Status::Ok;
      r.summary = input.vpnCountry.empty() ? input.vpnIp : std::format("{} · {}", input.vpnIp, input.vpnCountry);
      r.detail = std::format("Через VPN идут только выбранные программы и сайты — они выходят с этого адреса. "
                             "Остальные сайты видят твой: {}.",
                             seen);
      return r;
    }
    r.summary = seen;
    // Everything through the VPN: a site sees its exit - another address is the traffic going round it.
    if (!input.vpnIp.empty() && trace->ip != input.vpnIp &&
        (trace->loc.empty() || input.vpnCountry.empty() || trace->loc != input.vpnCountry)) {
      r.status = Status::Fail;
      r.detail = std::format("Сайты видят не адрес VPN ({}), а этот — трафик идёт мимо VPN.", input.vpnIp);
      r.fix = CheckFix::KillSwitch;
      return r;
    }
    r.status = Status::Ok;
    r.detail = "Этот адрес видят зарубежные сайты.";
    return r;
  }

  CheckResult RussiaDirect() {
    CheckResult r;
    const auto body = Download(L"https://ipv4-internet.yandex.net/api/v0/ip", input.userAgent, 4096);
    const auto ip = body ? ParseQuotedIp(*body) : std::nullopt;
    if (!ip) {
      r.status = Status::Warn;
      r.summary = "нет ответа";
      r.detail = "Российский сайт не ответил — проверить не вышло.";
      return r;
    }
    directIp = *ip;
    r.summary = *ip;
    const std::string vpn = VpnIp();
    const bool same = !vpn.empty() && *ip == vpn;
    if (input.selective) {
      r.status = Status::Ok;
      r.detail = same ? "Российские сайты идут через VPN — так решили твои правила."
                      : "Российские сайты видят твой адрес: в список VPN они не входят.";
      return r;
    }
    if (input.routing.russiaDirect) {
      r.status = same ? Status::Fail : Status::Ok;
      r.detail = same ? "Российские сайты видят адрес сервера, а не твой: правило «напрямую» не сработало."
                      : "Российские сайты видят твой адрес, не адрес сервера.";
      r.fix = same ? CheckFix::Routing : CheckFix::None;
    } else {
      r.status = Status::Ok;
      r.detail = same ? "«Российское напрямую» выключено: российские сайты видят адрес сервера."
                      : "Российские сайты видят этот адрес.";
    }
    return r;
  }

  CheckResult DnsLeak() {
    CheckResult r;
    const auto id = Download(L"https://bash.ws/id", input.userAgent, 256);
    const std::string token = id ? std::string(id->substr(0, id->find_first_of("\r\n "))) : std::string();
    if (token.empty() || token.size() > 64 ||
        !std::all_of(token.begin(), token.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); })) {
      r.status = Status::Warn;
      r.summary = "сервис проверки недоступен";
      r.detail = "bash.ws не ответил — попробуй позже.";
      return r;
    }
    for (int i = 1; i <= 8; ++i) {
      (void)Resolve(std::format("{}.{}.bash.ws", i, token));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const auto report = Download(Widen(std::format("https://bash.ws/dnsleak/test/{}?json", token)), input.userAgent,
                                 std::size_t{64} * 1024);
    return JudgeDnsLeak(report ? ParseDnsLeak(*report) : std::vector<DnsResolverSeen>{});
  }

  CheckResult WebRtc() const {
    CheckResult r;
    const auto mapped = StunMappedAddress();
    if (!mapped) {
      r.status = Status::Warn;
      r.summary = "UDP не проходит";
      r.detail = "STUN не ответил: звонки в браузере и игры через UDP могут не работать (утечки при этом нет).";
      return r;
    }
    r.summary = *mapped;
    const std::string vpn = VpnIp();
    if (input.selective) {
      r.status = Status::Ok;
      r.detail = *mapped == vpn ? "WebRTC видит адрес VPN."
                                : "Проверка идёт не из списка VPN и видит твой адрес — так и задумано. "
                                  "Программы из списка выдают через WebRTC адрес VPN.";
    } else if (!directIp.empty() && *mapped == directIp && *mapped != vpn) {
      r.status = Status::Fail;
      r.detail = "Утечка: через WebRTC сайты увидят твой настоящий адрес.";
      r.fix = CheckFix::KillSwitch;
    } else if (!vpn.empty() && *mapped != vpn) {
      r.status = Status::Warn;
      r.detail = "WebRTC видит адрес, отличный от адреса VPN — у UDP другой выход.";
    } else {
      r.status = Status::Ok;
      r.detail = "WebRTC видит адрес VPN — утечки нет.";
    }
    return r;
  }

  CheckResult Ipv6() const {
    CheckResult r;
    const auto trace = FetchTrace(L"https://[2606:4700:4700::1111]/cdn-cgi/trace");
    if (!trace) {
      r.status = Status::Ok;
      r.summary = "не используется";
      r.detail = "Трафик по IPv6 не уходит — мимо VPN тоже.";
      return r;
    }
    r.summary = trace->loc.empty() ? trace->ip : std::format("{} · {}", trace->ip, trace->loc);
    const std::string vpnLoc = VpnLoc();
    const bool proxied = trace->ip == VpnIp() || (!trace->loc.empty() && trace->loc == vpnLoc);
    if (input.selective) {
      r.status = Status::Ok;
      r.detail = proxied ? "IPv6 идёт через VPN." : "IPv6 идёт напрямую — как всё, что не в списке VPN.";
      return r;
    }
    r.status = proxied ? Status::Ok : Status::Fail;
    r.detail = proxied ? "IPv6 идёт через VPN." : "IPv6 уходит мимо VPN — сайты могут увидеть твой адрес.";
    r.fix = proxied ? CheckFix::None : CheckFix::KillSwitch;
    return r;
  }

  CheckResult Lan() const {
    CheckResult r;
    const auto [gateway, rtt] = PingGateway();
    if (gateway.empty()) {
      r.status = Status::Warn;
      r.summary = "роутер не найден";
      r.detail = "Нет адаптера со шлюзом — проверять нечего.";
      return r;
    }
    // Closed by the user: the router is meant to be silent unless let in.
    if (input.lanClosed) {
      const bool open =
          std::find(input.lanAllowed.begin(), input.lanAllowed.end(), gateway) != input.lanAllowed.end();
      if (!open) {
        r.status = rtt ? Status::Fail : Status::Ok;
        r.summary = rtt ? std::format("{} отвечает, хотя сеть закрыта", gateway) : std::string("закрыта");
        r.detail = rtt ? "Фильтр не держит: выключи и включи «Закрыть компьютер от сети» в настройках."
                       : "Компьютер закрыт от локальной сети (настройки), роутер в том числе — так и задумано. "
                         "Интернет это не трогает.";
        r.fix = rtt ? CheckFix::LocalNetwork : CheckFix::None;
        return r;
      }
      r.status = rtt ? Status::Ok : Status::Fail;
      r.summary = rtt ? std::format("{} · {} мс", gateway, *rtt) : std::format("{} не отвечает", gateway);
      r.detail = rtt ? "Локальная сеть закрыта, роутер открыт и отвечает."
                     : "Роутер открыт в настройках, но не отвечает.";
      return r;
    }
    if (!rtt) {
      r.status = Status::Fail;
      r.summary = std::format("{} не отвечает", gateway);
      r.detail = "Роутер не отвечает: связь с ним (Wi-Fi, кабель) или он сам.";
      return r;
    }
    r.status = Status::Ok;
    r.summary = std::format("{} · {} мс", gateway, *rtt);
    r.detail = "Роутер и локальная сеть доступны.";
    return r;
  }

  static CheckResult DnsLatency() {
    CheckResult r;
    // Names asked for the first time - the worst case: a name already asked
    // comes from the core's cache at once. One first, not counted: it may
    // reopen the connection to the DNS server through the proxy.
    const auto measure = [](const std::string& zone) -> std::optional<int> {
      (void)Resolve(std::format("sov-{}.{}", RandomLabel(10), zone));
      std::vector<int> times;
      for (int i = 0; i < 3; ++i) {
        if (const auto took = Resolve(std::format("sov-{}.{}", RandomLabel(10), zone))) {
          times.push_back(Millis(*took));
        }
      }
      if (times.empty()) {
        return std::nullopt;
      }
      std::sort(times.begin(), times.end());
      return times[times.size() / 2];
    };
    const auto foreign = measure("example.com");
    const auto russian = measure("yandex.ru");
    const auto show = [](const std::optional<int>& ms) { return ms ? std::format("{} мс", *ms) : std::string("нет ответа"); };
    r.summary = std::format("зарубежные {} · российские {}", show(foreign), show(russian));
    // Through the proxy a name takes about two trips to the server; directly, one to the DNS.
    const bool slow = foreign.value_or(0) > 600 || russian.value_or(0) > 300;
    r.status = !foreign || !russian ? Status::Fail : slow ? Status::Warn : Status::Ok;
    r.detail = r.status == Status::Fail ? "DNS не отвечает — сайты не откроются."
               : r.status == Status::Warn
                   ? "Новые имена узнаются медленно (знакомые берутся из кэша сразу). Попробуй другой DNS или сервер."
                   : "Новые имена: зарубежные — через VPN, российские — напрямую; знакомые — из кэша сразу.";
    r.fix = r.status == Status::Fail ? CheckFix::Dns : CheckFix::None;
    return r;
  }

  CheckResult Speed() const {
    CheckResult r;
    std::vector<int> pings;
    for (int i = 0; i < 5; ++i) {
      const auto started = Clock::now();
      if (Download(L"https://speed.cloudflare.com/__down?bytes=0", input.userAgent, 1024)) {
        pings.push_back(Millis(Clock::now() - started));
      }
    }
    // Each way for a few seconds, as speed tests do, the throughput taken
    // every quarter: more than the line can carry in that time is asked for.
    r.status = Status::Running;
    const auto show = [&] {
      r.summary = r.speedUp.empty() ? std::format("↓ {:.0f} Мбит/с…", r.speedDown.empty() ? 0.0 : r.speedDown.back())
                                    : std::format("↑ {:.0f} Мбит/с…", r.speedUp.back());
      if (live) {
        live(r);
      }
    };
    const auto down = Stream(kSpeedDownSeconds, r.speedDown, show);
    const auto up = UploadStream(kSpeedUpSeconds, r.speedUp, show);
    if (!down && !up) {
      r.status = Status::Fail;
      r.summary = "не удалось";
      r.detail = "Сервер замера недоступен: " + down.error();
      r.fix = CheckFix::Reconnect;
      return r;
    }
    std::sort(pings.begin(), pings.end());
    const std::string ping = pings.empty() ? std::string("—") : std::format("{} мс", pings[pings.size() / 2]);
    const std::string downText = down ? std::format("{:.0f}", StreamMbps(r.speedDown)) : std::string("—");
    const std::string upText = up ? std::format("{:.0f}", StreamMbps(r.speedUp)) : std::string("—");
    // Which server it went through: the core keeps the connections.
    std::vector<std::string> targets = Addresses("speed.cloudflare.com");
    targets.emplace_back("speed.cloudflare.com");
    const auto via = input.routeOf ? input.routeOf(targets) : std::nullopt;
    r.status = Status::Ok;
    r.summary = std::format("↓ {} · ↑ {} Мбит/с · {}", downText, upText, ping);
    const std::string way = !via                     ? std::string("Через VPN")
                            : via->type == "direct" ? std::string("Напрямую")
                                                    : "Через " + via->outbound;
    r.detail = way + " до ближайшего узла Cloudflare. Зависит от сервера и времени — сравни на странице «Серверы».";
    return r;
  }

  // The download: Cloudflare's test file read again and again for `seconds`,
  // as their own test does - from 1 MB, doubled while one comes in under
  // half a second, to 25 MB; a refusal (429 - too much asked lately; 403 -
  // over 50 MB) halves it. A stall ends a request after 3 s: another starts.
  // The throughput of every kSpeedSampleMs added to `samples`, `onSample`
  // told after each. Ok if anything came.
  std::expected<void, std::string> Stream(int seconds, std::vector<float>& samples,
                                          const std::function<void()>& onSample) const {
    const auto started = Clock::now();
    const auto until = started + std::chrono::seconds(seconds);
    Slots slots{.started = started, .sampledAt = started, .sampledBytes = 0};
    constexpr std::uint64_t kLeast = 1'000'000;
    constexpr std::uint64_t kMost = 25'000'000;
    std::uint64_t size = kLeast;
    std::uint64_t before = 0;  // what the requests before this one moved
    std::string error;
    while (Clock::now() < until) {
      const auto asked = Clock::now();
      const std::wstring url = std::format(L"https://speed.cloudflare.com/__down?bytes={}", size);
      const auto moved = StreamBytes(url, input.userAgent, 0, [&](std::uint64_t bytes) {
        const auto now = Clock::now();
        if (slots.Fill(samples, before + bytes, now)) {
          onSample();
        }
        return now < until;
      });
      if (!moved) {
        error = moved.error();  // tried again, smaller, while there's time - not in a tight loop
        size = std::max(kLeast, size / 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      before += *moved;
      if (*moved >= size && Clock::now() - asked < std::chrono::milliseconds(500)) {
        size = std::min(kMost, size * 2);
      }
    }
    if (before == 0) {
      return std::unexpected(error.empty() ? std::string("ничего не пришло") : error);
    }
    // Over before its first quarter (a tiny answer): its one sample.
    if (samples.empty() && before > 0) {
      samples.push_back(static_cast<float>(Mbps(before, Seconds(Clock::now() - started))));
      onSample();
    }
    return {};
  }

  // The upload: POSTs of a growing size, each timed to the server's answer -
  // the bytes it took, not the ones the system had buffered (written pieces
  // counted before they left: 15 and 305 Mbit/s in two runs of one line,
  // 16.10.2026). From 1 MB, doubled while one takes under half a second, to
  // 16 MB.
  std::expected<void, std::string> UploadStream(int seconds, std::vector<float>& samples,
                                                const std::function<void()>& onSample) const {
    const auto started = Clock::now();
    const auto until = started + std::chrono::seconds(seconds);
    Slots slots{.started = started, .sampledAt = started, .sampledBytes = 0};
    constexpr std::size_t kMost = std::size_t{16} * 1024 * 1024;
    const std::string body(kMost, 'x');
    std::size_t size = std::size_t{1024} * 1024;
    std::uint64_t total = 0;
    while (Clock::now() < until) {
      const auto sent = Clock::now();
      const auto done = Upload(L"https://speed.cloudflare.com/__up", input.userAgent, std::string_view(body).substr(0, size));
      const auto now = Clock::now();
      if (!done) {
        if (total == 0) {
          return std::unexpected(done.error());
        }
        break;
      }
      total += size;
      if (slots.Fill(samples, total, now)) {
        onSample();
      }
      if (now - sent < std::chrono::milliseconds(500) && size < kMost) {
        size *= 2;
      }
    }
    return {};
  }

  // A real request to `host` - the core sees the name (TLS SNI) and routes
  // it as any program's - and where the core says it went.
  SiteWay Visit(std::string host, std::string where, bool russian) const {
    SiteWay site{.host = std::move(host), .where = std::move(where), .russian = russian, .resolved = false, .way = std::nullopt};
    std::vector<std::string> targets = Addresses(site.host);
    if (targets.empty()) {
      return site;
    }
    site.resolved = true;
    targets.push_back(site.host);
    [[maybe_unused]] const auto answer =
        Download(Widen("https://" + site.host + "/"), input.userAgent, std::size_t{64} * 1024);
    site.way = input.routeOf ? input.routeOf(targets) : std::nullopt;
    return site;
  }

  // The address typed - or, with none, a Russian, a European and an American
  // site at once (kRouteProbes), judged against where each should go.
  CheckResult Route() const {
    if (input.host.empty()) {
      std::vector<SiteWay> sites;
      sites.reserve(kRouteProbes.size());
      for (const RouteProbe& probe : kRouteProbes) {
        sites.push_back(Visit(std::string(probe.host), std::string(probe.where), probe.russian));
      }
      return JudgeRoutes(sites, input.routing, input.selective);
    }
    std::string host = input.host;
    if (const std::size_t scheme = host.find("://"); scheme != std::string::npos) {
      host = host.substr(scheme + 3);
    }
    host = host.substr(0, host.find_first_of("/:?#"));
    const SiteWay site = Visit(host, {}, false);
    CheckResult r;
    r.status = site.way ? Status::Ok : Status::Warn;
    r.summary = host + ": " + (!site.resolved ? std::string("имя не находится")
                               : !site.way    ? std::string("не видно")
                                              : WayText(*site.way));
    r.detail = SiteLine(site, input.routing);
    return r;
  }
};

}  // namespace

void RunChecks(const std::stop_token& stop, const std::vector<CheckId>& which, const DiagnoseInput& input,
               const DiagnoseProgress& progress) {
  WSADATA wsa{};
  if (const int failed = WSAStartup(MAKEWORD(2, 2), &wsa); failed != 0) {
    for (const CheckId id : which) {
      progress(id, CheckResult{.status = Status::Fail,
                               .summary = "сеть недоступна",
                               .detail = std::format("Winsock не запустился (ошибка {}).", failed),
                               .fix = CheckFix::None,
                               .speedDown = {},
                               .speedUp = {}});
    }
    return;
  }
  Run run{.input = input, .exitIp = {}, .exitLoc = {}, .directIp = {}, .live = {}};
  for (const CheckId id : which) {
    if (stop.stop_requested()) {
      break;
    }
    progress(id, CheckResult{.status = Status::Running,
                             .summary = "проверяю...",
                             .detail = {},
                             .fix = CheckFix::None,
                             .speedDown = {},
                             .speedUp = {}});
    run.live = [&progress, id](const CheckResult& partial) { progress(id, partial); };
    CheckResult result;
    switch (id) {
      case CheckId::Exit: result = run.Exit(); break;
      case CheckId::RussiaDirect: result = run.RussiaDirect(); break;
      case CheckId::DnsLeak: result = run.DnsLeak(); break;
      case CheckId::WebRtc: result = run.WebRtc(); break;
      case CheckId::Ipv6: result = run.Ipv6(); break;
      case CheckId::Lan: result = run.Lan(); break;
      case CheckId::DnsLatency: result = Run::DnsLatency(); break;
      case CheckId::Speed: result = run.Speed(); break;
      case CheckId::Route: result = run.Route(); break;
    }
    progress(id, result);
  }
  WSACleanup();
}

}  // namespace sovereign::tray
