// The conformance harness (P3, issue #5): the ClientHello a client sends for
// one outbound, captured from several connections and compared - after
// canonicalization (src/conformance/client_hello.h) - with a golden ClientHello
// recorded from the pinned reference sing-box.
//
//   conformance-harness <case> --gocore <sovereign-gocore.dll> --golden <file> [--runs N]
//   conformance-harness <case> --binary <sing-box.exe>         --golden <file> [--runs N]
//   conformance-harness <case> --binary <sing-box.exe> --record <golden file> [--save <dir>] [--runs N]
//
// Both sides get a mixed inbound on loopback and the outbound under test
// pointed at this harness's listener, which records the first TLS record of
// every connection and closes it - nothing leaves the machine. Behind a
// "proxy" selector, as in a subscription.
//
// --binary is the reference: an executable run the way a user would
// (`sing-box run -c`), given the outbound as one would write it by hand.
//
// --gocore is Sovereign, the whole way a config travels in it, in this
// process (the service's pipe name is fixed, so a second service can't run
// next to an installed one - only the pipe transport itself is skipped):
//   - the outbound as Sovereign gets it: a share link through the tray's
//     importer (share_links.h) for cases that have one, else JSON as a
//     subscription delivers it - for AnyTLS also round-tripped through our
//     generated AnyTLSOutbound type (its `tls` object is OutboundTLSOptions,
//     OutboundUTLSOptions, OutboundRealityOptions), which checks the codegen;
//     the tray itself doesn't use the generated types yet;
//   - the tray's additions (effective_config.h: a protocol pick, a per-app
//     rule, a log level, the cache file) - what it sends to box_start;
//   - a box_start request through the service's ControlHandler, which
//     re-serializes the config, into GoCore and the DLL the service ships.
// A slip anywhere on that way that reshapes the TLS options shows up on the
// wire while the connection would still work.
//
// --record writes one raw capture as the golden file (after checking that all
// captures agree with each other), --save also writes every capture as
// <dir>/<case>.<n>.hex - the fixtures of conformance-client-hello.
//
// Not under ASan: the Go runtime and ASan's interceptors can't share a process
// (issue #9).

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <nlohmann/json.hpp>
#include <wil/resource.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "adapters/outbound_envelope.h"
#include "anytls_outbound.gen.h"
#include "client_hello.h"
#include "control.h"
#include "core.h"
#include "effective_config.h"
#include "go_core.h"
#include "log_ring.h"
#include "share_links.h"

namespace {

namespace fs = std::filesystem;
using sovereign::conformance::Canonical;
using sovereign::conformance::CanonicalOptions;
using sovereign::conformance::Canonicalize;
using sovereign::conformance::ClientHello;
using sovereign::conformance::Diff;
using sovereign::conformance::ParseClientHello;
using sovereign::conformance::Serialize;
using AnyTLSOutbound = sovereign::adapters::Outbound<sovereign::codegen::option::AnyTLSOutboundOptions>;
using Bytes = std::vector<std::uint8_t>;
using Json = nlohmann::json;

// A fixed throwaway REALITY public key: the client only needs a valid x25519
// point, and a fixed one keeps the config the same between runs.
constexpr std::string_view kRealityPublicKey = "jNXHt1yRo0vDuchQlIP6Z0ZvjT3KtzVI-T4E7RoLJS0";
constexpr int kDefaultRuns = 5;
constexpr int kWaitMs = 10000;
constexpr std::uint8_t kHandshakeRecord = 22;

constexpr std::string_view kServerName = "www.ebay.com";
constexpr std::string_view kShortId = "0123abcd";
constexpr std::string_view kVlessUuid = "b831381d-6324-4d53-ad4f-8cda48b30811";

enum class Protocol : std::uint8_t { AnyTLS, Vless };

struct Case {
  std::string_view name;
  Protocol protocol = Protocol::AnyTLS;
  bool reality = false;
  std::string_view fingerprint;
  CanonicalOptions canonical;
};

// uTLS HelloFirefox_120's GREASE ECH (u_parrots.go): CandidatePayloadLens
// {223} plus the 16-byte tag, AES-128-GCM or ChaCha20-Poly1305 per connection.
constexpr std::array<std::size_t, 1> kFirefoxEchPayloads{239};
constexpr std::array<std::uint16_t, 2> kFirefoxEchAeads{0x0001, 0x0003};

// uTLS chrome permutes its extensions per connection (client_hello.h).
// VLESS comes from a share link on our side, with a fingerprint that isn't
// the importer's REALITY default (chrome): an importer that dropped `fp`
// would still connect, and only the wire would tell.
constexpr std::array kCases{
    Case{.name = "anytls-reality-chrome",
         .reality = true,
         .fingerprint = "chrome",
         .canonical = {.extensionOrderRandomized = true}},
    Case{.name = "anytls-tls-chrome",
         .reality = false,
         .fingerprint = "chrome",
         .canonical = {.extensionOrderRandomized = true}},
    Case{.name = "vless-reality-firefox",
         .protocol = Protocol::Vless,
         .reality = true,
         .fingerprint = "firefox",
         .canonical = {.extensionOrderRandomized = false,
                       .echPayloadLengths = kFirefoxEchPayloads,
                       .echAeads = kFirefoxEchAeads}},
};

class Winsock {
 public:
  Winsock() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
  }
  ~Winsock() { WSACleanup(); }
  Winsock(const Winsock&) = delete;
  Winsock& operator=(const Winsock&) = delete;
  Winsock(Winsock&&) = delete;
  Winsock& operator=(Winsock&&) = delete;
};

sockaddr_in Loopback(std::uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  return address;
}

// A listening socket on a loopback port the system picks.
wil::unique_socket Listen(std::uint16_t& port) {
  wil::unique_socket s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  sockaddr_in address = Loopback(0);
  int length = sizeof address;
  if (!s || bind(s.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 ||
      listen(s.get(), SOMAXCONN) != 0 || getsockname(s.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    throw std::runtime_error(std::format("cannot listen on loopback: {}", WSAGetLastError()));
  }
  port = ntohs(address.sin_port);
  return s;
}

// A loopback port free right now, for the core's mixed inbound.
std::uint16_t FreePort() {
  std::uint16_t port = 0;
  Listen(port);  // closed again on return
  return port;
}

std::optional<wil::unique_socket> Connect(std::uint16_t port) {
  wil::unique_socket s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  const sockaddr_in address = Loopback(port);
  if (!s || connect(s.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
    return std::nullopt;
  }
  return s;
}

// The core started: its inbound accepts connections.
void WaitForPort(std::uint16_t port) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
  while (!Connect(port)) {
    if (std::chrono::steady_clock::now() > deadline) {
      throw std::runtime_error(std::format("nothing listens on the inbound port {}", port));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// The first TLS record the peer sends; nullopt if it isn't a handshake record.
std::optional<Bytes> FirstRecord(SOCKET s) {
  const DWORD timeout = kWaitMs;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
  Bytes data;
  std::array<char, 4096> buffer{};
  const auto wanted = [&] { return data.size() < 5 ? 5 : 5 + ((static_cast<std::size_t>(data[3]) << 8U) | data[4]); };
  while (data.size() < wanted()) {
    const int n = recv(s, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (n <= 0) {
      break;
    }
    data.insert(data.end(), buffer.begin(), buffer.begin() + n);
  }
  if (data.size() < 5 || data[0] != kHandshakeRecord || data.size() < wanted()) {
    return std::nullopt;
  }
  data.resize(wanted());
  return data;
}

// One connection through the inbound per run; the outbound under test dials
// the listener, which keeps its first record and hangs up.
std::vector<Bytes> Capture(SOCKET listener, std::uint16_t inboundPort, int runs) {
  std::vector<Bytes> records;
  for (int n = 0; n < runs; ++n) {
    auto client = Connect(inboundPort);
    if (!client) {
      throw std::runtime_error("the inbound refused a connection");
    }
    constexpr std::string_view request = "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n";
    send(client->get(), request.data(), static_cast<int>(request.size()), 0);
    WSAPOLLFD poll{.fd = listener, .events = POLLRDNORM, .revents = 0};
    if (WSAPoll(&poll, 1, kWaitMs) != 1) {
      throw std::runtime_error(std::format("run {}: the outbound never dialed the listener", n + 1));
    }
    const wil::unique_socket connection(accept(listener, nullptr, nullptr));
    auto record = FirstRecord(connection.get());
    if (!record) {
      throw std::runtime_error(std::format("run {}: the first record isn't a TLS handshake", n + 1));
    }
    records.push_back(std::move(*record));
  }
  return records;
}

std::string Utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

// The outbound under test as one would write it for sing-box by hand.
Json PlainOutbound(const Case& c, std::uint16_t capturePort) {
  Json tls = Json::object();
  tls["enabled"] = true;
  tls["server_name"] = kServerName;
  tls["utls"] = {{"enabled", true}, {"fingerprint", c.fingerprint}};
  if (c.reality) {
    tls["reality"] = {{"enabled", true}, {"public_key", kRealityPublicKey}, {"short_id", kShortId}};
  }
  Json outbound = Json::object();
  outbound["tag"] = "out";
  outbound["server"] = "127.0.0.1";
  outbound["server_port"] = capturePort;
  if (c.protocol == Protocol::Vless) {
    outbound["type"] = "vless";
    outbound["uuid"] = kVlessUuid;
    outbound["flow"] = "xtls-rprx-vision";
    outbound["packet_encoding"] = "xudp";
  } else {
    outbound["type"] = "anytls";
    outbound["password"] = "conformance";
  }
  outbound["tls"] = std::move(tls);
  return outbound;
}

// The outbound the way it reaches Sovereign (see the top of the file).
Json SovereignOutbound(const Case& c, std::uint16_t capturePort) {
  if (c.protocol == Protocol::Vless) {
    const std::string link = std::format(
        "vless://{}@127.0.0.1:{}?encryption=none&flow=xtls-rprx-vision&security=reality&sni={}&fp={}&pbk={}&sid={}"
        "&type=tcp#conformance",
        kVlessUuid, capturePort, kServerName, c.fingerprint, kRealityPublicKey, kShortId);
    const auto parsed = sovereign::tray::ParseShareLink(link);
    if (!parsed.outbound) {
      throw std::runtime_error("the share link didn't import: " + parsed.error);
    }
    Json outbound = Json::parse(*parsed.outbound);
    outbound["tag"] = "out";
    return outbound;
  }
  return Json(PlainOutbound(c, capturePort).get<AnyTLSOutbound>());
}

Json Config(Json outbound, std::uint16_t inboundPort) {
  Json inbound = Json::object();
  inbound["type"] = "mixed";
  inbound["tag"] = "in";
  inbound["listen"] = "127.0.0.1";
  inbound["listen_port"] = inboundPort;

  Json selector = Json::object();
  selector["type"] = "selector";
  selector["tag"] = "proxy";
  selector["outbounds"] = Json::array({"out"});

  Json config = Json::object();
  // The listener hangs up on every connection by design, so every run would
  // log a "failed to create session: EOF" error; only fatal ones matter here
  // (a box that doesn't start fails Start() with its reason anyway).
  config["log"] = {{"level", "fatal"}};
  config["inbounds"] = Json::array({inbound});
  config["outbounds"] = Json::array({selector, std::move(outbound)});
  config["route"] = {{"final", "proxy"}};
  return config;
}

// What the tray would send for that config, with a user's settings: a
// protocol pick, an app rule and the cache file. The app rule names no real
// program, so the harness's own connections still go through the proxy. No
// log level: it can't reach an outbound, and at any level the tray offers
// every run would log the listener's hang-up.
std::string SovereignConfig(const Case& c, std::uint16_t capturePort, std::uint16_t inboundPort,
                            const fs::path& cacheFile) {
  return sovereign::tray::EffectiveConfig(Config(SovereignOutbound(c, capturePort), inboundPort).dump(),
                                          {.protocol = "out",
                                           .appsMode = sovereign::tray::AppsMode::Exclude,
                                           .apps = {"not-this-harness.exe"},
                                           .cacheFile = Utf8(cacheFile)});
}

class Driver {
 public:
  Driver() = default;
  virtual ~Driver() = default;
  Driver(const Driver&) = delete;
  Driver& operator=(const Driver&) = delete;
  Driver(Driver&&) = delete;
  Driver& operator=(Driver&&) = delete;

  virtual void Start(const std::string& config) = 0;
  virtual void Stop() = 0;
  virtual std::string_view Name() const = 0;
};

// The service's side of box_start/box_stop: its ControlHandler over GoCore,
// fed the requests the tray would send down the pipe.
class GoCoreDriver final : public Driver {
 public:
  explicit GoCoreDriver(const fs::path& dll) : core_(dll.wstring()) {
    core_.SetLogSink([](sovereign::service::LogLevel level, std::string_view message) {
      if (level <= sovereign::service::LogLevel::Fatal) {
        std::cerr << "  core: " << message << "\n";
      }
    });
  }
  // Straight to the core: Handle() builds JSON and may throw, a destructor
  // mustn't.
  ~GoCoreDriver() override { core_.Stop(); }
  GoCoreDriver(const GoCoreDriver&) = delete;
  GoCoreDriver& operator=(const GoCoreDriver&) = delete;
  GoCoreDriver(GoCoreDriver&&) = delete;
  GoCoreDriver& operator=(GoCoreDriver&&) = delete;

  void Start(const std::string& config) override {
    Json request = Json::object();
    request["cmd"] = "box_start";
    request["config"] = Json::parse(config);  // as the tray's StartBox sends it
    const Json response = Json::parse(control_.Handle(request.dump()));
    if (response.value("cmd", "") != "box_started") {
      throw std::runtime_error("box_start: " + response.value("message", response.dump()));
    }
  }
  void Stop() override { control_.Handle(R"({"cmd":"box_stop"})"); }
  std::string_view Name() const override { return "gocore"; }

 private:
  static constexpr std::size_t kLogCapacity = 64;  // the ring is the service's; nothing here reads it

  sovereign::service::GoCore core_;
  sovereign::service::LogRing log_{kLogCapacity};
  std::optional<std::string> lastConfig_;
  sovereign::service::ControlHandler control_{&core_, log_, lastConfig_};
};

class BinaryDriver final : public Driver {
 public:
  explicit BinaryDriver(fs::path binary) : binary_(std::move(binary)) {}
  ~BinaryDriver() override { Stop(); }
  BinaryDriver(const BinaryDriver&) = delete;
  BinaryDriver& operator=(const BinaryDriver&) = delete;
  BinaryDriver(BinaryDriver&&) = delete;
  BinaryDriver& operator=(BinaryDriver&&) = delete;

  void Start(const std::string& config) override {
    configPath_ = fs::temp_directory_path() / std::format("sovereign-conformance-{}.json", GetCurrentProcessId());
    std::ofstream(configPath_) << config;
    std::wstring command = L"\"" + binary_.wstring() + L"\" run -c \"" + configPath_.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
      throw std::runtime_error(std::format("cannot start {}: {}", binary_.string(), GetLastError()));
    }
    process_.reset(process.hProcess);
    CloseHandle(process.hThread);
  }

  void Stop() override {
    if (process_) {
      TerminateProcess(process_.get(), 0);
      WaitForSingleObject(process_.get(), kWaitMs);
      process_.reset();
    }
    std::error_code ignored;
    fs::remove(configPath_, ignored);
  }

  std::string_view Name() const override { return "binary"; }

 private:
  fs::path binary_;
  fs::path configPath_;
  wil::unique_handle process_;
};

std::string Hex(const Bytes& bytes) {
  std::string out;
  for (const std::uint8_t b : bytes) {
    out += std::format("{:02x}", b);
  }
  return out;
}

Bytes ReadHex(const fs::path& path) {
  std::ifstream file(path);
  std::string hex;
  file >> hex;
  if (hex.empty() || hex.size() % 2 != 0) {
    throw std::runtime_error("not a hex record: " + path.string());
  }
  Bytes out;
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

Canonical CanonicalOf(const Bytes& record, const Case& c) {
  const auto hello = ParseClientHello(record);
  if (!hello) {
    throw std::runtime_error("unparsable ClientHello: " + hello.error());
  }
  return Canonicalize(*hello, c.canonical);
}

// Differences of every capture from `expected`, printed; how many captures differ.
int Report(const ClientHello& expected, const std::vector<Bytes>& records, const Case& c, std::string_view against) {
  int failed = 0;
  for (std::size_t n = 0; n < records.size(); ++n) {
    const Canonical ours = CanonicalOf(records[n], c);
    const auto diff = Diff(expected, ours.hello);
    if (diff.empty() && ours.violations.empty()) {
      continue;
    }
    ++failed;
    std::cout << std::format("  run {} differs from {}:\n", n + 1, against);
    for (const std::string& line : diff) {
      std::cout << "    " << line << "\n";
    }
    for (const std::string& line : ours.violations) {
      std::cout << "    violation: " << line << "\n";
    }
  }
  return failed;
}

struct Options {
  const Case* testCase = nullptr;
  std::optional<fs::path> gocore;
  std::optional<fs::path> binary;
  std::optional<fs::path> golden;
  std::optional<fs::path> record;
  std::optional<fs::path> save;
  int runs = kDefaultRuns;
};

Options Parse(const std::vector<std::string>& args) {
  Options o;
  if (args.empty()) {
    throw std::invalid_argument("no case");
  }
  for (const Case& c : kCases) {
    if (c.name == args[0]) {
      o.testCase = &c;
    }
  }
  if (o.testCase == nullptr) {
    throw std::invalid_argument("unknown case " + args[0]);
  }
  for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
    const std::string& key = args[i];
    const fs::path value = args[i + 1];
    if (key == "--gocore") {
      o.gocore = value;
    } else if (key == "--binary") {
      o.binary = value;
    } else if (key == "--golden") {
      o.golden = value;
    } else if (key == "--record") {
      o.record = value;
    } else if (key == "--save") {
      o.save = value;
    } else if (key == "--runs") {
      o.runs = std::stoi(args[i + 1]);
    } else {
      throw std::invalid_argument("unknown option " + key);
    }
  }
  if (o.gocore.has_value() == o.binary.has_value() || o.golden.has_value() == o.record.has_value() || o.runs < 2) {
    throw std::invalid_argument("need one of --gocore/--binary, one of --golden/--record, and --runs >= 2");
  }
  return o;
}

int Run(const Options& o) {
  const Case& c = *o.testCase;
  const Winsock winsock;
  std::uint16_t capturePort = 0;
  const wil::unique_socket listener = Listen(capturePort);
  const std::uint16_t inboundPort = FreePort();

  const fs::path cacheFile = fs::temp_directory_path() / std::format("sovereign-conformance-{}.db", GetCurrentProcessId());
  std::unique_ptr<Driver> driver;
  std::string config;
  if (o.gocore) {
    driver = std::make_unique<GoCoreDriver>(*o.gocore);
    config = SovereignConfig(c, capturePort, inboundPort, cacheFile);
  } else {
    driver = std::make_unique<BinaryDriver>(*o.binary);
    config = Config(PlainOutbound(c, capturePort), inboundPort).dump();
  }
  driver->Start(config);
  WaitForPort(inboundPort);
  const std::vector<Bytes> records = Capture(listener.get(), inboundPort, o.runs);
  driver->Stop();
  std::error_code ignored;
  fs::remove(cacheFile, ignored);
  std::cout << std::format("{} ({}): {} ClientHellos captured\n", c.name, driver->Name(), records.size());

  // The captures agree with each other first: otherwise the canonical form is
  // missing a mask and no comparison with anything else means much.
  const Canonical first = CanonicalOf(records.front(), c);
  if (Report(first.hello, records, c, "run 1") != 0) {
    std::cout << "FAIL: the client's own connections don't canonicalize equal\n";
    return 1;
  }

  if (o.record) {
    std::ofstream(*o.record) << Hex(records.front()) << "\n";
    if (o.save) {
      fs::create_directories(*o.save);
      for (std::size_t n = 0; n < records.size(); ++n) {
        std::ofstream(*o.save / std::format("{}.{}.hex", c.name, n + 1)) << Hex(records[n]) << "\n";
      }
    }
    std::cout << "recorded " << o.record->string() << "\n";
    return 0;
  }

  const Canonical golden = CanonicalOf(ReadHex(*o.golden), c);
  const int failed = Report(golden.hello, records, c, "the golden ClientHello");
  if (failed != 0) {
    std::cout << std::format("FAIL: {} of {} ClientHellos differ from the golden one\n", failed, records.size());
    return 1;
  }
  std::cout << "OK: every ClientHello equals the golden one, byte for byte outside the masks\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    const Options options = Parse(std::vector<std::string>(argv + 1, argv + argc));
    return Run(options);
  } catch (const std::invalid_argument& e) {
    std::cerr << "usage: conformance-harness <case> (--gocore <dll> | --binary <exe>) (--golden <file> | --record <file> [--save <dir>]) [--runs N]\n"
              << "error: " << e.what() << "\n";
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
