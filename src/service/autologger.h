#pragma once

// The service's flight recorder: an ETW AutoLogger session that Windows starts
// at every boot and that writes Sovereign.Core's events (trace.h) to
// %ProgramData%\Sovereign\Logs\sovereign.etl - circular, 16 MB, the files of
// the last kAutoLoggerFileMax boots kept as sovereign.etl.NNNN, flushed every
// second. Kernel-owned: events the service emitted survive the service
// crashing. Default keywords only - sing-box's info lines (domains) stay out.
//
// Registered by `sovereign-core --install`, which also starts the same
// session right away so logging doesn't wait for the next reboot; removed by
// --uninstall (the .etl files are left for whoever needs them).
// Read with tools/trace/sovtrace.ps1.

namespace sovereign::service {

inline constexpr wchar_t kAutoLoggerName[] = L"Sovereign-Core";

// Both need admin (they're called from --install / --uninstall). Throw
// wil::ResultException on failure.
void InstallAutoLogger();
void UninstallAutoLogger();

}  // namespace sovereign::service
