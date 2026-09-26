#pragma once

#include <windows.h>

#include <wil/resource.h>

#include "tray_model.h"

namespace sovereign::tray {

// The tray icon for a state: a filled circle, drawn at the system's small-icon
// size (so it follows DPI) - no resource files to keep in sync.
//   ServiceDown grey, Off dark grey, Starting amber, On green, Error red.
wil::unique_hicon MakeStateIcon(Display display);

}  // namespace sovereign::tray
