#include "icons.h"

#include <wil/result.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sovereign::tray {

namespace {

COLORREF StateColor(Display display) {
  switch (display) {
    case Display::ServiceDown: return RGB(150, 150, 150);
    case Display::Off: return RGB(90, 90, 90);
    case Display::Starting: return RGB(235, 165, 0);
    case Display::On: return RGB(40, 175, 80);
    case Display::Error: return RGB(215, 50, 50);
  }
  return RGB(150, 150, 150);
}

}  // namespace

wil::unique_hicon MakeStateIcon(Display display) {
  const int size = GetSystemMetrics(SM_CXSMICON);
  const COLORREF color = StateColor(display);

  // A top-down 32-bit DIB with premultiplied alpha: the circle's edge is
  // antialiased by coverage, the rest is transparent.
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof header;
  header.bV5Width = size;
  header.bV5Height = -size;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;

  void* bits = nullptr;
  const wil::unique_hdc_window screen = wil::GetDC(nullptr);
  wil::unique_hbitmap colorBitmap(CreateDIBSection(screen.get(), reinterpret_cast<const BITMAPINFO*>(&header),
                                                   DIB_RGB_COLORS, &bits, nullptr, 0));
  THROW_LAST_ERROR_IF(!colorBitmap || !bits);

  auto* pixels = static_cast<std::uint32_t*>(bits);
  const double center = (size - 1) / 2.0;
  const double radius = size / 2.0 - 1.0;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const double distance = std::hypot(x - center, y - center);
      const double coverage = std::clamp(radius - distance + 0.5, 0.0, 1.0);
      const auto a = static_cast<std::uint32_t>(std::lround(coverage * 255.0));
      const auto r = static_cast<std::uint32_t>(std::lround(GetRValue(color) * coverage));
      const auto g = static_cast<std::uint32_t>(std::lround(GetGValue(color) * coverage));
      const auto b = static_cast<std::uint32_t>(std::lround(GetBValue(color) * coverage));
      pixels[static_cast<std::size_t>(y) * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }

  // With a 32-bit alpha color bitmap the mask is ignored, but must exist.
  wil::unique_hbitmap mask(CreateBitmap(size, size, 1, 1, nullptr));
  THROW_LAST_ERROR_IF(!mask);

  ICONINFO info{};
  info.fIcon = TRUE;
  info.hbmColor = colorBitmap.get();
  info.hbmMask = mask.get();
  wil::unique_hicon icon(CreateIconIndirect(&info));
  THROW_LAST_ERROR_IF(!icon);
  return icon;
}

}  // namespace sovereign::tray
