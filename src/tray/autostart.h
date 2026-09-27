#pragma once

namespace sovereign::tray {

// Starting the tray when the user signs in: a value under the user's
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run - no admin rights, and
// Task Manager's Startup tab shows it and can turn it off like any other.
// The service needs nothing of the kind, the SCM starts it.

// Whether the Run value exists (whatever path it holds).
bool AutostartEnabled();

// Writes the Run value pointing at this exe, or deletes it. False if the
// registry refused.
bool SetAutostart(bool enabled);

}  // namespace sovereign::tray
