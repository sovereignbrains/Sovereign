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
#include <format>
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

struct Run {
  const DiagnoseInput& input;
  std::string exitIp;   // what foreign sites see
  std::string exitLoc;
  std::string directIp;  // what Russian ones see

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
      return r;
    }
    exitIp = trace->ip;
    exitLoc = trace->loc;
    r.status = Status::Ok;
    r.summary = trace->loc.empty() ? trace->ip : std::format("{} · {}", trace->ip, trace->loc);
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
    const bool same = !exitIp.empty() && *ip == exitIp;
    if (input.routing.russiaDirect) {
      r.status = same ? Status::Fail : Status::Ok;
      r.detail = same ? "Российские сайты видят адрес сервера, а не твой: правило «напрямую» не сработало."
                      : "Российские сайты видят твой адрес, не адрес сервера.";
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
    if (!directIp.empty() && *mapped == directIp && *mapped != exitIp) {
      r.status = Status::Fail;
      r.detail = "Утечка: через WebRTC сайты увидят твой настоящий адрес.";
    } else if (!exitIp.empty() && *mapped != exitIp) {
      r.status = Status::Warn;
      r.detail = "WebRTC видит адрес, отличный от адреса прокси — у UDP другой выход.";
    } else {
      r.status = Status::Ok;
      r.detail = "WebRTC видит адрес прокси — утечки нет.";
    }
    return r;
  }

  CheckResult Ipv6() const {
    CheckResult r;
    const auto trace = FetchTrace(L"https://[2606:4700:4700::1111]/cdn-cgi/trace");
    if (!trace) {
      r.status = Status::Ok;
      r.summary = "не используется";
      r.detail = "Трафик по IPv6 не уходит — мимо туннеля тоже.";
      return r;
    }
    r.summary = trace->loc.empty() ? trace->ip : std::format("{} · {}", trace->ip, trace->loc);
    const bool proxied = trace->ip == exitIp || (!trace->loc.empty() && trace->loc == exitLoc);
    r.status = proxied ? Status::Ok : Status::Fail;
    r.detail = proxied ? "IPv6 идёт через прокси." : "IPv6 уходит мимо прокси — сайты могут увидеть твой адрес.";
    return r;
  }

  static CheckResult Lan() {
    CheckResult r;
    const auto [gateway, rtt] = PingGateway();
    if (gateway.empty()) {
      r.status = Status::Warn;
      r.summary = "роутер не найден";
      r.detail = "Нет адаптера со шлюзом — проверять нечего.";
      return r;
    }
    if (!rtt) {
      r.status = Status::Fail;
      r.summary = std::format("{} не отвечает", gateway);
      r.detail = "Локальная сеть недоступна. Если включён kill switch — включи в нём «Локальная сеть».";
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
                   : "Новые имена: зарубежные — через прокси, российские — напрямую; знакомые — из кэша сразу.";
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
    constexpr std::uint64_t kDown = 25'000'000;
    const auto downStarted = Clock::now();
    const auto down = Download(Widen(std::format("https://speed.cloudflare.com/__down?bytes={}", kDown)), input.userAgent,
                               kDown + 1024);
    const double downSeconds = Seconds(Clock::now() - downStarted);
    const std::string upBody(8'000'000, 'x');
    const auto upStarted = Clock::now();
    const auto up = Upload(L"https://speed.cloudflare.com/__up", input.userAgent, upBody);
    const double upSeconds = Seconds(Clock::now() - upStarted);
    if (!down && !up) {
      r.status = Status::Fail;
      r.summary = "не удалось";
      r.detail = "Сервер замера недоступен: " + down.error();
      return r;
    }
    std::sort(pings.begin(), pings.end());
    const std::string ping = pings.empty() ? std::string("—") : std::format("{} мс", pings[pings.size() / 2]);
    const std::string downText = down ? std::format("{:.0f}", Mbps(down->size(), downSeconds)) : std::string("—");
    const std::string upText = up ? std::format("{:.0f}", Mbps(upBody.size(), upSeconds)) : std::string("—");
    // Which server it went through: the core's log says.
    std::vector<std::string> targets = Addresses("speed.cloudflare.com");
    targets.emplace_back("speed.cloudflare.com");
    const auto via = OutboundInLogs(input.logs ? input.logs() : std::vector<std::string>{}, targets);
    r.status = Status::Ok;
    r.summary = std::format("↓ {} · ↑ {} Мбит/с · {}", downText, upText, ping);
    r.detail = (via ? (*via == "direct" ? std::string("Напрямую") : "Через " + *via) : std::string("Через прокси")) +
               " до ближайшего узла Cloudflare. Зависит от сервера и времени — сравни на странице «Серверы».";
    return r;
  }

  CheckResult Route() const {
    CheckResult r;
    std::string host = input.host;
    if (const std::size_t scheme = host.find("://"); scheme != std::string::npos) {
      host = host.substr(scheme + 3);
    }
    host = host.substr(0, host.find_first_of("/:?#"));
    const std::string rule = ClientRuleFor(host, input.routing);
    std::vector<std::string> targets = Addresses(host);
    if (targets.empty()) {
      r.status = Status::Warn;
      r.summary = host + ": имя не находится";
      r.detail = rule.empty() ? "Заблокировано DNS-правилом (реклама) или такого сайта нет."
                              : "Подходит: " + rule + ". Имя не находится — заблокировано или сайта нет.";
      return r;
    }
    targets.push_back(host);
    // A real request: the core sees the name (TLS SNI) and routes as for any program.
    [[maybe_unused]] const auto answer = Download(Widen("https://" + host + "/"), input.userAgent, std::size_t{64} * 1024);
    std::optional<std::string> outbound;
    for (int i = 0; i < 4 && !outbound; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(700));  // the worker collects the core's log every second
      outbound = OutboundInLogs(input.logs ? input.logs() : std::vector<std::string>{}, targets);
    }
    if (!outbound) {
      r.status = Status::Warn;
      r.summary = host + ": в журнале не видно";
      r.detail = "Соединение не попало в журнал ядра: уровень записи ниже info или сайт заблокирован." +
                 (rule.empty() ? std::string() : " Подходит: " + rule + ".");
      return r;
    }
    r.status = Status::Ok;
    r.summary = *outbound == "direct" ? host + " → напрямую" : host + " → через " + *outbound;
    r.detail = rule.empty() ? "Ни одно из твоих правил не подошло — решили списки или «остальное»."
                            : "Сработало: " + rule + ".";
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
                               .detail = std::format("Winsock не запустился (ошибка {}).", failed)});
    }
    return;
  }
  Run run{.input = input, .exitIp = {}, .exitLoc = {}, .directIp = {}};
  for (const CheckId id : which) {
    if (stop.stop_requested()) {
      break;
    }
    progress(id, CheckResult{.status = Status::Running, .summary = "проверяю...", .detail = {}});
    CheckResult result;
    switch (id) {
      case CheckId::Exit: result = run.Exit(); break;
      case CheckId::RussiaDirect: result = run.RussiaDirect(); break;
      case CheckId::DnsLeak: result = run.DnsLeak(); break;
      case CheckId::WebRtc: result = run.WebRtc(); break;
      case CheckId::Ipv6: result = run.Ipv6(); break;
      case CheckId::Lan: result = Run::Lan(); break;
      case CheckId::DnsLatency: result = Run::DnsLatency(); break;
      case CheckId::Speed: result = run.Speed(); break;
      case CheckId::Route: result = run.Route(); break;
    }
    progress(id, result);
  }
  WSACleanup();
}

}  // namespace sovereign::tray
