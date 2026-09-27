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

**Tray** (`sovereign-tray.exe`) — a Windows 11-style flyout next to the notification-area icon:

- on/off, with live speed and connection count;
- **subscription**: paste an `https://` link from the clipboard, refresh by hand or on the server's `Profile-Update-Interval`; the server sees a `sing-box` User-Agent with the pinned version;
- **protocol**: pick any option of the subscription's selector (or `auto`);
- **per-app routing**: everything except a list of programs, or only the list;
- start at sign-in, the main window.

**Main window** — everything the flyout does, with room: an overview (on/off, speed and connections, a two-minute speed graph), the protocol pick, the subscription (server, last refresh, interval, errors), per-app routing, the core's log (levels in color, lines select and copy), settings. Drawn with Direct2D like the flyout, keyboard-navigable (Tab, Enter, Ctrl+1..6). Started by hand the tray opens it; from the Run key (`--background`) it stays in the notification area; a second start brings the running tray's window up. Closing it only hides it.

**Service** (`sovereign-core.exe`, runs as SYSTEM):

- hosts sing-box (`sovereign-gocore.dll`, the pinned version built with upstream's Windows release tags) behind an `ICore` interface;
- TUN through wintun; system DNS settings are never touched — DNS is hijacked inside the route;
- restarts the box after sleep; keeps a cache file so remote rule-sets don't block a start without network;
- logs through ETW (TraceLogging provider `Sovereign.Core`) with an always-on flight recorder.

## How it fits together

```
sovereign-tray.exe  (user session)                sovereign-core.exe  (Windows service, SYSTEM)
  flyout, subscription, per-app rules,
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
src/tray/         the tray: flyout and main window (Direct2D), worker, subscription, per-app rules
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
- the pinned sources: `git clone --depth 1 --branch v1.14.1 https://github.com/SagerNet/sing-box vendor/sing-box` (tag from `tools/codegen/sing-box.version`), and `vendor/wintun/amd64/wintun.dll` from the zip pinned in `tools/codegen/wintun.version`.

From a Developer Command Prompt:

```
cmake --preset ci
cmake --build --preset ci
ctest --test-dir build/ci --output-on-failure
```

Presets: `ci` (Debug), `ci-asan`, `ci-analyze`, `ci-fuzz` (then `pwsh tools/fuzz/run.ps1`), `release`. To also run the conformance reference locally, configure with `-DSOVEREIGN_REFERENCE_SINGBOX=<path to the official sing-box.exe of the pinned version>`.

## Running

```
sovereign-core.exe --install     (admin) register and start the service and its ETW recorder
sovereign-core.exe --uninstall   (admin) remove both
sovereign-core.exe --run         run in a console instead, for debugging
sovereign-tray.exe               the tray and its window; copy a subscription link and use "paste"
```

Settings and the downloaded config live in `%LOCALAPPDATA%\Sovereign`, the service's logs and cache in `%ProgramData%\Sovereign`. `tools/trace/sovtrace.ps1 dump` decodes the flight recorder.

## License

MIT — see [LICENSE](LICENSE).
