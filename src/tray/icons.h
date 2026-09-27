#pragma once

// windows.h's min/max macros break std::min/max: the CI's clang-tidy doesn't see
// the build's global NOMINMAX (see src/common/sha256.h), so every header that
// pulls windows.h in sets it itself.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wil/resource.h>

#include "tray_model.h"

namespace sovereign::tray {

// The tray icon for a state, at the system's small-icon size (so it follows
// DPI): the app icon with a state dot in the corner - ServiceDown grey, Off
// dark grey, Starting amber, On green, Error red. Just the dot, full size, if
// the app icon resource can't be read.
wil::unique_hicon MakeStateIcon(Display display);

// The app icon (tray.rc) at `size` pixels; null if it can't be loaded.
wil::unique_hicon LoadAppIcon(int size);

// The state's color (the icon's, and the flyout's status dot).
COLORREF StateColor(Display display);

}  // namespace sovereign::tray
