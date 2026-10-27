# Sovereign

[![CI](https://github.com/sovereignbrains/Sovereign/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/sovereignbrains/Sovereign/actions/workflows/ci.yml)

A native Windows client for [sing-box](https://github.com/SagerNet/sing-box), written in C++23 — and built so that its quality can be checked rather than claimed.

Third-party sing-box clients have a reputation for being thin, sloppy wrappers. Sovereign's answer is evidence in CI:

- **Servers are written from generated types, not by hand.** Every outbound and WireGuard endpoint the client writes (share links, subscriptions, WARP) is a C++ option struct generated from the pinned sing-box sources by `tools/codegen` (real Go type-checking, not regex; sing-box's discriminated unions read from its own schema). CI regenerates them and fails on any drift from upstream, and the import tests require our servers to come out of `sing-box format` unchanged - and to read back through the generated structs the same. The routing frame around them (DNS, rules, TUN) is still assembled by hand.
- **What goes on the wire is compared with the reference, byte for byte.** A conformance harness captures the TLS ClientHello Sovereign's core sends and the one the official sing-box release sends, and diffs them outside the fields that are random by design.
- **Clean C++.** RAII only, no `new`/`delete`, warnings as errors, MSVC `/analyze`, clang-tidy, AddressSanitizer, ThreadSanitizer, fuzzing.

## Status

The client works and is in daily use; the project is between its MVP and its conformance phase. Plan and gates: [#8 Roadmap](https://github.com/sovereignbrains/Sovereign/issues/8).

| Phase | What | State |
|---|---|---|
| P0 | CMake + vcpkg, Windows service, named pipe, CI | done — [#1](https://github.com/sovereignbrains/Sovereign/issues/1) |
| P1 | Option structs generated from sing-box, golden tests | done — [#2](https://github.com/sovereignbrains/Sovereign/issues/2) |
| P2 | Hybrid MVP: sing-box as a DLL inside the service, TUN, tray | feature-complete; acceptance on a clean Windows 11 and a week of daily use (G2) pending — [#3](https://github.com/sovereignbrains/Sovereign/issues/3) |
| P2.5 | Per-UA subscription, ETW logging, system DNS left alone | done — [#4](https://github.com/sovereignbrains/Sovereign/issues/4) |
| P3 | Conformance harness against the pinned reference | minimum met: byte-identical ClientHello for AnyTLS and AnyTLS+REALITY in CI; fuzzing and ThreadSanitizer in CI — [#5](https://github.com/sovereignbrains/Sovereign/issues/5) |
| P4 | NativeCore (own TLS/transport), optional | not started — [#6](https://github.com/sovereignbrains/Sovereign/issues/6) |
| P5 | Upstream the harness, release | not started — [#7](https://github.com/sovereignbrains/Sovereign/issues/7) |

## What it does today

The interface is in Russian. Paste a subscription, press the switch, forget about it — and everything past that is a click away when it's wanted.

**The main screen** — a cube for a switch (its colour and pose are the connection's state), the traffic drawn live around it (through the VPN, directly, through WARP, ads blocked on its face), the server in use with its country's flag (a click opens the servers), and a card per configuration: its name and till when it's paid, with "+" to add another (the clipboard, a QR code on the screen, a file). The title bar holds the tools (the speed test, the checks, where an address goes, the log) and the settings. A click on the notification-area icon opens the window by it; a right click gives on/off, open, the log, quit.

**Configurations** — subscriptions, sets of keys and config files, as many as you add, every one that's on running together: their servers go into one list with `auto`. Anything a provider or another client hands out is taken: an `https://` link (bare or inside an app's deep link), a whole sing-box config, proxy keys (`vless`, `vmess`, `trojan`, `ss`, `hysteria2`, `hysteria`, `tuic`, `anytls`, `socks`, `naive`, `wireguard`, `ssh`, `snell` — on lines, glued together, percent-encoded or base64), Clash/Mihomo YAML, Xray/v2rayN JSON, SIP008, or a picture of QR codes. Every config built that way passes the pinned sing-box's `check` in CI. A subscription server sees as little as can be: a `sing-box 1.x.y` User-Agent, an `x-hwid` that is random per configuration (never the machine's), and the VPN's address rather than yours; with no connection up, an optional relay (your own Cloudflare Worker, `tools/relay`) fetches it instead. A provider's "auto" balancer servers are hidden — the tray picks. When a subscription is in trouble (unpaid, out of traffic, its servers silent), the main screen says so and offers a report for the provider's support: only what concerns their service, nothing about you.

**Servers** — each one's latency measured so that it means something: one connection, then requests over it until the round trips settle, shown as the median ± half their spread with losses (sing-box's own URL test is one cold request). `auto` takes the best and moves only once another has been clearly better twice in a row, or at once if the one in use stops answering. Switching never restarts the connection.

**Routing and DNS are the client's** — a configuration gives servers; the TUN, the DNS and the rules are Sovereign's. On a board of four ways — through the VPN, directly, through WARP, blocked — sit services (28 of them: Netflix, YouTube, ChatGPT, Telegram — its desktop app by its IP ranges too —, Steam and Epic games…; a service can also go through a country — the fastest server whose exit is there), programs, sites, Russian sites, ads, cookie banners and the local network. Everything through the VPN but the exceptions, or only what's chosen. WARP is a Cloudflare WireGuard endpoint the client registers itself, over the VPN or directly. Fine settings, folded: QUIC (blocked automatically when the servers can't carry it as UDP), Hysteria 2's Brutal speed, IPv4 only, the DNS for sites through the VPN and for direct ones (encrypted, auto by default, or your own: plain, TLS, HTTPS, QUIC). The lists are SagerNet's `sing-geosite`/`sing-geoip` and AdGuard's, fetched by the tray, refreshed daily and given to the core as local files; every combination is checked against the pinned sing-box in CI. The config stays yours to edit: over your edits a new version of the subscription waits, and you take it, keep yours or carry your edits over (a three-way merge).

**Protection** — a kill switch (on by default): while the connection is meant to be on nothing leaves the machine but through the VPN, and if the service falls over or the machine reboots, the internet stays shut until it's back. The local network can be closed both ways, the router included, with the devices you choose let through. With "start and connect" the service brings the tunnel up at boot, before anyone signs in — no browser in the Startup folder gets there first.

**Checks** — the tray's own requests go through the TUN and the rules like any program's, so they meet what your programs meet: the VPN's exit, what Russian sites see, a DNS leak test (bash.ws), WebRTC (a STUN request, as a browser's), IPv6, the local network, DNS latency, where the Russian, European and American sites go (asked of the core's own connection tracker). They know the mode: with only chosen things through the VPN, a site going directly is as meant, not a leak. A failed one offers its way out — turn the kill switch on, reconnect, open the routing or the DNS. The speed test streams each way for a few seconds against Cloudflare and draws a graph as it goes. `diagnose-probe` runs the same checks from a console.

**Log** — the core's lines in colour, filters by level, pause, save, a line in full on a click, scrolled with the middle button as in a browser; noise the core reports as errors (a stream closed without one) shown as what it is.

**Updates and uninstalling** — the tray checks for a new release and updates in place (the installer checked against the release's `.sha256`). Uninstalling leaves nothing behind: the service, the filters, the logs, the cache, the boot config and the downloaded updates always go; the subscriptions and settings (your servers' keys) too, unless you say keep them.

Drawn with Direct2D, keyboard-navigable (Tab, Enter, Esc, Ctrl+1..5 for the main screen, servers, configurations, log and settings). Started by hand the tray opens the window; from the Run key (`--background`) it stays in the notification area. Closing the window only hides it.

**Service** (`sovereign-core.exe`, runs as SYSTEM):

- hosts sing-box (`sovereign-gocore.dll`, the pinned version built with upstream's Windows release tags) behind an `ICore` interface;
- TUN through wintun; system DNS settings are never touched — DNS is hijacked inside the route;
- enforces the kill switch and the closed local network with Windows Filtering Platform filters of its own (persistent, in their own sublayer);
- starts the config kept for boot by itself (DPAPI-encrypted, readable by SYSTEM and Administrators only), restarts the box after sleep, keeps a cache file so remote rule sets don't block a start without network;
- answers the checks from the core's traffic manager — where the newest connection to an address went, by which rule;
- logs through ETW (TraceLogging provider `Sovereign.Core`) with an always-on flight recorder.

## How it fits together

```
sovereign-tray.exe  (user session)                sovereign-core.exe  (Windows service, SYSTEM)
  window, configurations, routing,
  builds the effective config         ──JSON over \\.\pipe\sovereign-control──►  ControlHandler
                                                                                   │ ICore
                                                                                   ▼
                                                                          GoCore → sovereign-gocore.dll
                                                                                   (sing-box, c-shared)
                                                                                   │
                                                                          TUN (wintun) → outbounds
```

The tray owns the state (settings, configurations, routing) and sends the service a complete config; the service keeps only the one it's asked to start at boot. The pipe is the only link between them, and there is no pipe into the core — `ICore` is an in-process interface, so a native core (P4) can replace the Go one without changing anything around it.

## Repository layout

```
src/service/      the service: pipe server, control protocol, GoCore bridge, kill switch (WFP), boot config, ETW
src/tray/         the tray: its window (Direct2D), worker, configurations, routing, checks
src/common/       shared headers and the handwritten JSON adapters (Duration, Listable, ...)
src/generated/    C++ option structs generated from sing-box — do not edit, regenerate
src/conformance/  ClientHello parser and canonical form for the harness
gocore/           the Go side: sing-box built as a c-shared DLL with a C ABI
installer/        Inno Setup script and its build and smoke scripts
tools/codegen/    Go -> C++ option-struct generator; pinned versions (sing-box, wintun)
tools/goldengen/  emits golden JSON through sing-box's own marshaler
tools/relay/      the subscription relay: a Cloudflare Worker
tools/trace/      sovtrace.ps1 — start/stop/dump the ETW recording
tests/unit/       unit tests (control protocol, tray logic, adapters)
tests/golden/     generated structs and adapters against golden JSON
tests/conformance/  the harness, golden ClientHellos, captured fixtures
tests/fuzz/       libFuzzer targets, seed corpora, the JSON dictionary (run with tools/fuzz/run.ps1)
tests/tsan/       the control plane's and the tray's threads raced under ThreadSanitizer
tests/ui/         ui-snapshot: the window's pages drawn into PNGs and laid out as text, no desktop needed
tests/smoke/      service smoke tests run by CI (GoCore, ETW, flight recorder)
```

## How quality is checked

Every push (and a nightly run) goes through eight jobs:

- **build** — Debug build with warnings as errors, all tests, the real GoCore smoke test, the ETW flight-recorder test under the real SCM, the conformance harness against the official sing-box release (downloaded and sha256-pinned), the window's pages drawn to PNG (the `ui-snapshots` artifact) and laid out as text (clickable things overlapping or past the edge fail), and typing in the real window driven by keys;
- **analyze** — the whole tree under MSVC `/analyze`;
- **asan** — everything under AddressSanitizer (the Go runtime can't share an ASan process, so GoCore's C++ side is exercised against a stub DLL with the same C ABI — [#9](https://github.com/sovereignbrains/Sovereign/issues/9));
- **tsan** — the service's control plane and the tray's logic, built on Linux with clang, raced by threads under ThreadSanitizer;
- **clang-tidy** — with the clang-tidy bundled with the runner's Visual Studio;
- **fuzz** — four libFuzzer targets (MSVC `/fsanitize=fuzzer` with ASan), a minute each per push and twenty minutes each nightly: the ClientHello parser, the generated config structs and adapters, everything the tray reads that it didn't write (subscriptions, service answers), and the service's control protocol — what any local user can send a SYSTEM service. Besides "no crash" each checks properties: exact round trips, exactly one JSON answer per request. Seeds live in `tests/fuzz/corpus`, every past finding among them;
- **codegen-drift** — the option structs regenerated from the pinned sing-box: any difference from what's committed fails;
- **installer** — the installer built, then installed, updated and uninstalled on the runner.

The conformance harness in short: five connections of the same client differ on the wire (randoms, GREASE, key shares, Chrome's extension order, ECH GREASE), so the harness first proves that the reference agrees with itself after masking exactly those fields — a mutation test checks that every other byte is compared — and only then compares Sovereign's core with it. Details: the plan and measurements in [#5](https://github.com/sovereignbrains/Sovereign/issues/5).

Stubs are allowed only with a name, a reason and a destination: generated fields that still need a handwritten adapter say so in `src/generated/*.gen.h`.

## Building

Needs Windows 10/11 x64, and:

- Visual Studio 2022 Build Tools (C++ workload; the Clang tools component for clang-tidy), CMake ≥ 3.28, Ninja;
- vcpkg at `C:\vcpkg`;
- Go (see `gocore/go.mod`) and a MinGW-w64 `gcc` on `PATH` — cgo needs it for the DLL;
- the pinned sources: `git clone --depth 1 --branch v1.14.4 https://github.com/SagerNet/sing-box vendor/sing-box` (tag from `tools/codegen/sing-box.version`), and `vendor/wintun/amd64/wintun.dll` from the zip pinned in `tools/codegen/wintun.version`.

From a Developer Command Prompt:

```
cmake --preset ci
cmake --build --preset ci
ctest --test-dir build/ci --output-on-failure
```

Presets: `ci` (Debug), `ci-asan`, `ci-analyze`, `ci-fuzz` (then `pwsh tools/fuzz/run.ps1`), `release`. To also run the conformance reference locally, configure with `-DSOVEREIGN_REFERENCE_SINGBOX=<path to the official sing-box.exe of the pinned version>`. The desktop test (typing in the real window) runs in CI by itself; locally set `SOVEREIGN_DESKTOP_TESTS=1`.

## Installing

Download `Sovereign-Setup-<version>.exe` from [Releases](https://github.com/sovereignbrains/Sovereign/releases) and run it. It installs into Program Files, registers the service (starts with Windows, restarts itself after a crash), adds Sovereign to the Start menu and starts it; uninstall from Settings → Apps. The tray checks for a newer release a minute after start and every 12 hours, and updates in place when asked: it downloads the new installer, checks it against the release's `.sha256` and runs it (Windows asks for administrator rights). Your settings in `%LOCALAPPDATA%\Sovereign` survive updates; uninstalling asks whether to keep them.

Releases are built by `.github/workflows/release.yml`: push a tag `vX.Y.Z` (or run the workflow with the version), and it builds the installer (`installer/build.ps1`: Release, the CRT linked in, Inno Setup), installs, updates and uninstalls it on the runner (`installer/smoke.ps1`) and publishes it with its checksum.

## Running from a build

```
sovereign-core.exe --install     (admin) register and start the service and its ETW recorder
sovereign-core.exe --uninstall   (admin) remove both, the kill switch and the boot config
sovereign-core.exe --unblock     (admin) lift the kill switch by hand, if the service can't
sovereign-core.exe --run         run in a console instead, for debugging
sovereign-tray.exe               the tray and its window; copy a subscription link and use "+"
```

Settings and the configurations live in `%LOCALAPPDATA%\Sovereign`; the service's logs, cache, rule sets and boot config in `%ProgramData%\Sovereign`. `tools/trace/sovtrace.ps1 dump` decodes the flight recorder.

## License

MIT — see [LICENSE](LICENSE).
