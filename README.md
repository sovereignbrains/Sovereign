# Sovereign

[![CI](https://github.com/sovereignbrains/Sovereign/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/sovereignbrains/Sovereign/actions/workflows/ci.yml)

A native Windows client for [sing-box](https://github.com/SagerNet/sing-box), written in C++23 — and built so that its quality can be checked rather than claimed.

Third-party sing-box clients have a reputation for being thin, sloppy wrappers. Sovereign's answer is evidence in CI:

- **The config model is generated, not handwritten.** C++ option structs come from the pinned sing-box sources by `tools/codegen` (real Go type-checking, not regex), and every serializer is tested against JSON that sing-box's own marshaler produced.
- **What goes on the wire is compared with the reference, byte for byte.** A conformance harness captures the TLS ClientHello Sovereign's core sends and the one the official sing-box release sends, and diffs them outside the fields that are random by design.
- **Clean C++.** RAII only, no `new`/`delete`, warnings as errors, MSVC `/analyze`, clang-tidy, AddressSanitizer.

## Status

The client works and is in daily use; the project is between its MVP and its conformance phase. Plan and gates: [#8 Roadmap](https://github.com/sovereignbrains/Sovereign/issues/8).

| Phase | What | State |
|---|---|---|
| P0 | CMake + vcpkg, Windows service, named pipe, CI | done — [#1](https://github.com/sovereignbrains/Sovereign/issues/1) |
| P1 | Option structs generated from sing-box, golden tests | done — [#2](https://github.com/sovereignbrains/Sovereign/issues/2) |
| P2 | Hybrid MVP: sing-box as a DLL inside the service, TUN, tray | feature-complete; acceptance on a clean Windows 11 and a week of daily use (G2) pending — [#3](https://github.com/sovereignbrains/Sovereign/issues/3) |
| P2.5 | Per-UA subscription, ETW logging, system DNS left alone | done — [#4](https://github.com/sovereignbrains/Sovereign/issues/4) |
| P3 | Conformance harness against the pinned reference | minimum met: byte-identical ClientHello for AnyTLS and AnyTLS+REALITY in CI; fuzzing in CI; TSan next — [#5](https://github.com/sovereignbrains/Sovereign/issues/5) |
| P4 | NativeCore (own TLS/transport), optional | not started — [#6](https://github.com/sovereignbrains/Sovereign/issues/6) |
| P5 | Upstream the harness, release | not started — [#7](https://github.com/sovereignbrains/Sovereign/issues/7) |

## What it does today

**Tray** (`sovereign-tray.exe`) — one compact window (about 400×620), opened by a click on the notification-area icon, in the corner by it; a right click gives a short menu (on/off, open, log, quit). Its main screen: the state and speed in a line at the top, tiles into the subscription, the apps, the log and the settings, and a bar at the bottom with the on/off button, the server in use - a click on it opens the servers - and the exit IP with its country's flag (a click hides the address, leaving the country; looked up through the server itself, via Cloudflare's trace):

- on/off, with live speed and connection count;
- **configurations**: as many as you add - subscriptions, sets of keys, config files - listed on the subscription page, each with an on/off switch. **All that are on run together**: the first one on gives the frame (its inbounds, DNS, routing, rule sets) and every other one adds its servers to the frame's proxy selector and its URL tests, so the server list and `auto` cover them all (tags that clash get the configuration's name). A click opens a configuration's page: rename it, turn auto-update on or off with any interval (the server's `Profile-Update-Interval`, a preset, or any number of hours), refresh, copy the link, carry edits over a new version, remove it (its `config.json` is kept in `history\`) - and its servers with their protocols (`VLESS · REALITY`, `Trojan · WS`...), each with a switch: a server switched off leaves the config with whatever was chained through it and groups left empty, and whatever pointed at it (rules, DNS detours, rule-set downloads, `route.final`) points at the selector instead. Every combined config is checked against the pinned sing-box in CI. Each configuration has its own folder (`profiles\<id>\`), named by the panel's `Profile-Title` until you rename it. Adding never replaces: a link already there is switched on and refreshed, the same keys again switch theirs on. A tray from before this keeps its configuration;
- **routing and DNS are the client's**: in "own" mode (the default) Sovereign builds the TUN, DNS and routing itself and the configurations only give servers; in "subscription" mode one chosen configuration's routing and DNS stay. Either way the client's rules come first: **Russia directly** (on by default - a Russian site or app that sees the proxy server's address can tie it to you): the .ru/.рф/.su/... zones, the `category-ru`/`gov-ru` lists and Russian IPs go around the proxy, and their names resolve directly; ads blocked; QUIC blocked (browsers fall back to TCP, steadier through a proxy); your own rules - sites, IP ranges, programs - directly, through the proxy or blocked, added in place, and "copy from a subscription". DNS: remote through the proxy (Cloudflare/Google/Quad9 over HTTPS), direct names and the servers' own over encrypted DNS straight from here (Cloudflare or Google over HTTPS, or the system's), IPv4 only by default. The lists are SagerNet's `sing-geosite`/`sing-geoip` rule sets, downloaded by the tray and refreshed daily, given to the core as local files - a list that can't be fetched never stops a start (the zones still go directly meanwhile). Every combination is checked against the pinned sing-box in CI;
- **adding one**: paste from the clipboard, open a file or scan the screen for a QR code - whatever it holds: an `https://` link (bare, or inside an app's deep link: `sing-box://import-remote-profile`, `clash://install-config`, `hiddify://import/`, `sub://`...), a whole sing-box config, proxy keys (`vless://`, `vmess://`, `trojan://`, `ss://`, `hysteria2://`/`hy2://`, `hysteria://`, `tuic://`, `anytls://`, `socks://`, `naive+https://`, `wireguard://`/`wg://`, `ssh://`, `snell://`: one or many, on lines, spaced, glued together, percent-encoded, in HTML or base64-encoded), other clients' formats (Clash/Mihomo YAML, Xray/v2rayN JSON, SIP008, bare sing-box outbounds), or a picture of QR codes (a screenshot or an image on the clipboard, an image file, several codes at once); servers become a config of your own, and every config built that way passes the pinned sing-box's `check` in CI; a subscription server that answers in any of these formats instead of a sing-box config works too, copy the link to share it, unsubscribe; refresh by hand or on the server's `Profile-Update-Interval`; the server sees a `sing-box` User-Agent with the pinned version;
- **the config is yours to edit**: the subscription is kept as it arrived (`subscription.json`) and `config.json` starts as its copy. A new version replaces an unedited config silently; over your edits it waits (`subscription.new.json`) and you choose — carry your edits over to it (a three-way merge: objects key by key, tagged lists like `outbounds` by tag, route rules keeping your additions and removals in place; where both sides changed the same thing yours wins and it's listed), take the new one, or keep yours. "Back to the subscription" drops the edits. Whatever gets replaced goes to `history\` first (the last 20);
- **servers**: pick any option of the subscription's selector (or `auto`), with each server's latency - sing-box's own URL test through every option, run on each connect and on demand;
- **per-app routing**: everything except a list of programs, or only the list; each shown with its own icon (the exe path is remembered when it is added, or found among running processes);
- **log**: the core's lines with their levels in color; filters by level (with counts), pause (new lines wait, counted), a click shows the line in full, right-click to copy, select all, save to a .txt or clear; what the core writes at all (debug/info/warn/error, or the config's) is picked there too;
- **kill switch** (the overview and settings): while the connection is on, nothing goes out except through the proxy - if the core or the service falls over, or the machine reboots mid-session, the internet stays closed until it's back (the window says so); turning the connection off opens it. The local network (printers, NAS) can stay reachable, DNS there excepted;
- start at sign-in, updates.

Every page (servers, subscription, apps, the log and settings) opens in the window's place, back with the arrow or Esc. What waits for a click (a new version, a subscription meeting your edits) is one line above the tiles. Drawn with Direct2D, keyboard-navigable (Tab, Enter, Ctrl+1..6, Esc). Started by hand the tray opens the window; from the Run key (`--background`) it stays in the notification area; a second start brings the running tray's window up. Closing it only hides it.

**Service** (`sovereign-core.exe`, runs as SYSTEM):

- hosts sing-box (`sovereign-gocore.dll`, the pinned version built with upstream's Windows release tags) behind an `ICore` interface;
- TUN through wintun; system DNS settings are never touched — DNS is hijacked inside the route;
- enforces the kill switch with Windows Filtering Platform filters of its own (persistent, in their own sublayer): only `sovereign-core.exe`, loopback, DHCP, the TUN's addresses and optionally the LAN get out;
- restarts the box after sleep; keeps a cache file so remote rule-sets don't block a start without network;
- logs through ETW (TraceLogging provider `Sovereign.Core`) with an always-on flight recorder.

## How it fits together

```
sovereign-tray.exe  (user session)                sovereign-core.exe  (Windows service, SYSTEM)
  window, subscription, per-app rules,
  builds the effective config         ──JSON over \\.\pipe\sovereign-control──►  ControlHandler
                                                                                   │ ICore
                                                                                   ▼
                                                                          GoCore → sovereign-gocore.dll
                                                                                   (sing-box, c-shared)
                                                                                   │
                                                                          TUN (wintun) → outbounds
```

The tray owns all state (settings, the subscription's config) and sends the service a complete config; the service keeps none. The pipe is the only link between them, and there is no pipe into the core — `ICore` is an in-process interface, so a native core (P4) can replace the Go one without changing anything around it.

## Repository layout

```
src/service/      the service: pipe server, control protocol, GoCore bridge, ETW, power events
src/tray/         the tray: its window (Direct2D), worker, subscription, per-app rules
src/common/       shared headers and the handwritten JSON adapters (Duration, Listable, ...)
src/generated/    C++ option structs generated from sing-box — do not edit, regenerate
src/conformance/  ClientHello parser and canonical form for the harness
gocore/           the Go side: sing-box built as a c-shared DLL with a C ABI
tools/codegen/    Go -> C++ option-struct generator; pinned versions (sing-box, wintun)
tools/goldengen/  emits golden JSON through sing-box's own marshaler
tools/trace/      sovtrace.ps1 — start/stop/dump the ETW recording
tests/unit/       unit tests (control protocol, tray logic, adapters)
tests/golden/     generated structs and adapters against golden JSON
tests/conformance/  the harness, golden ClientHellos, captured fixtures
tests/fuzz/       libFuzzer targets, seed corpora, the JSON dictionary (run with tools/fuzz/run.ps1)
tests/ui/         ui-snapshot: the main window's pages drawn into PNGs with sample content, no desktop needed
tests/smoke/      service smoke tests run by CI (GoCore, ETW, flight recorder)
```

## How quality is checked

Every push (and a nightly run) goes through five jobs on `windows-latest`:

- **build** — Debug build with warnings as errors, all tests, the real GoCore smoke test, the ETW flight-recorder test under the real SCM, the conformance harness against the official sing-box release (downloaded and sha256-pinned), and the main window's pages drawn to PNG (the `ui-snapshots` artifact);
- **analyze** — the whole tree under MSVC `/analyze`;
- **asan** — everything under AddressSanitizer (the Go runtime can't share an ASan process, so GoCore's C++ side is exercised against a stub DLL with the same C ABI — [#9](https://github.com/sovereignbrains/Sovereign/issues/9));
- **clang-tidy** — with the clang-tidy bundled with the runner's Visual Studio;
- **fuzz** — four libFuzzer targets (MSVC `/fsanitize=fuzzer` with ASan), a minute each per push and twenty minutes each nightly: the ClientHello parser, the generated config structs and adapters, everything the tray reads that it didn't write (subscriptions, service answers), and the service's control protocol — what any local user can send a SYSTEM service. Besides "no crash" each checks properties: exact round trips, exactly one JSON answer per request. Seeds live in `tests/fuzz/corpus`, every past finding among them.

The conformance harness in short: five connections of the same client differ on the wire (randoms, GREASE, key shares, Chrome's extension order, ECH GREASE), so the harness first proves that the reference agrees with itself after masking exactly those fields — a mutation test checks that every other byte is compared — and only then compares Sovereign's core with it. Details: the plan and measurements in [#5](https://github.com/sovereignbrains/Sovereign/issues/5).

Stubs are allowed only with a name, a reason and a destination: generated fields that still need a handwritten adapter say so in `src/generated/*.gen.h`.

## Building

Needs Windows 10/11 x64, and:

- Visual Studio 2022 Build Tools (C++ workload; the Clang tools component for clang-tidy), CMake ≥ 3.28, Ninja;
- vcpkg at `C:\vcpkg`;
- Go (see `gocore/go.mod`) and a MinGW-w64 `gcc` on `PATH` — cgo needs it for the DLL;
- the pinned sources: `git clone --depth 1 --branch v1.14.2 https://github.com/SagerNet/sing-box vendor/sing-box` (tag from `tools/codegen/sing-box.version`), and `vendor/wintun/amd64/wintun.dll` from the zip pinned in `tools/codegen/wintun.version`.

From a Developer Command Prompt:

```
cmake --preset ci
cmake --build --preset ci
ctest --test-dir build/ci --output-on-failure
```

Presets: `ci` (Debug), `ci-asan`, `ci-analyze`, `ci-fuzz` (then `pwsh tools/fuzz/run.ps1`), `release`. To also run the conformance reference locally, configure with `-DSOVEREIGN_REFERENCE_SINGBOX=<path to the official sing-box.exe of the pinned version>`.

## Installing

Download `Sovereign-Setup-<version>.exe` from [Releases](https://github.com/sovereignbrains/Sovereign/releases) and run it. It installs into Program Files, registers the service (starts with Windows, restarts itself after a crash), adds Sovereign to the Start menu and starts it; uninstall from Settings → Apps. The tray checks for a newer release a minute after start and every 12 hours, and updates in place when asked: it downloads the new installer, checks it against the release's `.sha256` and runs it (Windows asks for administrator rights). Your settings in `%LOCALAPPDATA%\Sovereign` survive updates and uninstalling.

Releases are built by `.github/workflows/release.yml`: push a tag `vX.Y.Z` (or run the workflow with the version), and it builds the installer (`installer/build.ps1`: Release, the CRT linked in, Inno Setup), installs, updates and uninstalls it on the runner (`installer/smoke.ps1`) and publishes it with its checksum. The CI's `installer` job does the same on every push, without publishing.

## Running from a build

```
sovereign-core.exe --install     (admin) register and start the service and its ETW recorder
sovereign-core.exe --uninstall   (admin) remove both, and the kill switch
sovereign-core.exe --unblock     (admin) lift the kill switch by hand, if the service can't
sovereign-core.exe --run         run in a console instead, for debugging
sovereign-tray.exe               the tray and its window; copy a subscription link and use "paste"
```

Settings and the downloaded config live in `%LOCALAPPDATA%\Sovereign`, the service's logs and cache in `%ProgramData%\Sovereign`. `tools/trace/sovtrace.ps1 dump` decodes the flight recorder.

## License

MIT — see [LICENSE](LICENSE).
