#pragma once

// windows.h's min/max macros break std::min/max: the CI's clang-tidy doesn't see
// the build's global NOMINMAX (see src/common/sha256.h), so every header that
// pulls windows.h in sets it itself.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wil/resource.h>

#include <cstdint>
#include <string_view>
#include <vector>

#include "tray_model.h"

namespace sovereign::tray {

// The tray icon for a state, at the system's small-icon size (so it follows
// DPI): the app icon with a state dot in the corner - ServiceDown grey, Off
// dark grey, Starting amber, On green, Error red. Just the dot, full size, if
// the app icon resource can't be read.
wil::unique_hicon MakeStateIcon(Display display);

// The app icon (tray.rc) at `size` pixels; null if it can't be loaded.
wil::unique_hicon LoadAppIcon(int size);

// A service's logo (ServiceCatalog's id: the resource SVC_<ID>, from
// assets/services) drawn at size pixels with Direct2D's SVG: premultiplied
// BGRA, top-down, size * size; empty if there's none or it can't be drawn.
// COM must be initialized on the thread (WIC).
std::vector<std::uint32_t> ServiceLogo(std::string_view id, int size);

// ServiceLogo as a menu item's bitmap (32-bit with alpha); null if none.
wil::unique_hbitmap ServiceMenuIcon(std::string_view id, int size);
// The state's color (the icon's).
COLORREF StateColor(Display display);

}  // namespace sovereign::tray
