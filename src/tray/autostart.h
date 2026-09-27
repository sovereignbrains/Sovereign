#pragma once

namespace sovereign::tray {

// Starting the tray when the user signs in: a value under the user's
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run - no admin rights, and
// Task Manager's Startup tab shows it and can turn it off like any other.
// The service needs nothing of the kind, the SCM starts it.

// The argument the Run value passes: start in the notification area only.
// Started without it (a shortcut, a double click) the tray opens its window.
inline constexpr wchar_t kBackgroundArg[] = L"--background";

// Whether the Run value exists (whatever path it holds).
bool AutostartEnabled();

// Writes the Run value pointing at this exe with kBackgroundArg, or deletes
// it. False if the registry refused.
bool SetAutostart(bool enabled);

// An enabled Run value written by an older tray (no kBackgroundArg, so the
// window would open at every sign-in) is rewritten; anything else is left as
// it is.
void RefreshAutostart();

}  // namespace sovereign::tray
