#include "icons.h"

#include <wil/result.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "resource.h"

namespace sovereign::tray {

namespace {

// Straight (non-premultiplied) BGRA, one uint32 per pixel, top-down - what
// icon color bitmaps hold and what CreateIconIndirect takes back.
using Pixels = std::vector<std::uint32_t>;

BITMAPV5HEADER DibHeader(int size) {
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
  return header;
}

// The app icon's pixels at `size`, or empty if the resource can't be read.
Pixels AppIconPixels(int size) {
  const wil::unique_hicon icon = LoadAppIcon(size);
  if (!icon) {
    return {};
  }
  ICONINFO info{};
  if (!GetIconInfo(icon.get(), &info)) {
    return {};
  }
  const wil::unique_hbitmap color(info.hbmColor);
  const wil::unique_hbitmap mask(info.hbmMask);
  BITMAPV5HEADER header = DibHeader(size);
  Pixels pixels(static_cast<std::size_t>(size) * size);
  const wil::unique_hdc_window screen = wil::GetDC(nullptr);
  if (!color || GetDIBits(screen.get(), color.get(), 0, static_cast<UINT>(size), pixels.data(),
                          reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS) != size) {
    return {};
  }
  return pixels;
}

// `color` at `alpha` over the pixel, straight alpha.
void Over(std::uint32_t& pixel, COLORREF color, double alpha) {
  if (alpha <= 0) {
    return;
  }
  const double below = static_cast<double>((pixel >> 24) & 0xFF) / 255.0;
  const double out = alpha + below * (1 - alpha);
  const auto channel = [&](int shift, double source) {
    const auto dest = static_cast<double>((pixel >> shift) & 0xFF);
    return static_cast<std::uint32_t>(std::lround((source * alpha + dest * below * (1 - alpha)) / out));
  };
  const std::uint32_t r = channel(16, GetRValue(color));
  const std::uint32_t g = channel(8, GetGValue(color));
  const std::uint32_t b = channel(0, GetBValue(color));
  pixel = (static_cast<std::uint32_t>(std::lround(out * 255.0)) << 24) | (r << 16) | (g << 8) | b;
}

// A filled circle, edge antialiased by coverage.
void Disc(Pixels& pixels, int size, double cx, double cy, double radius, COLORREF color) {
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const double coverage = std::clamp(radius - std::hypot(x - cx, y - cy) + 0.5, 0.0, 1.0);
      Over(pixels[static_cast<std::size_t>(y) * size + x], color, coverage);
    }
  }
}

wil::unique_hicon IconFromPixels(const Pixels& pixels, int size) {
  const BITMAPV5HEADER header = DibHeader(size);
  void* bits = nullptr;
  const wil::unique_hdc_window screen = wil::GetDC(nullptr);
  wil::unique_hbitmap colorBitmap(CreateDIBSection(screen.get(), reinterpret_cast<const BITMAPINFO*>(&header),
                                                   DIB_RGB_COLORS, &bits, nullptr, 0));
  THROW_LAST_ERROR_IF(!colorBitmap || !bits);
  std::copy(pixels.begin(), pixels.end(), static_cast<std::uint32_t*>(bits));

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

}  // namespace

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

wil::unique_hicon LoadAppIcon(int size) {
  return wil::unique_hicon(static_cast<HICON>(
      LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_SOVEREIGN), IMAGE_ICON, size, size, 0)));
}

wil::unique_hicon MakeStateIcon(Display display) {
  const int size = GetSystemMetrics(SM_CXSMICON);
  const COLORREF color = StateColor(display);
  Pixels pixels = AppIconPixels(size);
  if (pixels.empty()) {
    // No app icon (it's a resource of the exe): the state alone, a full disc.
    pixels.assign(static_cast<std::size_t>(size) * size, 0);
    Disc(pixels, size, (size - 1) / 2.0, (size - 1) / 2.0, size / 2.0 - 1.0, color);
    return IconFromPixels(pixels, size);
  }
  // The app icon with the state as a dot in the bottom-right corner, ringed
  // in the panel's dark color so it reads on light and dark taskbars alike.
  const double radius = std::max(2.5, size * 0.24);
  const double center = size - radius - 1.0;
  Disc(pixels, size, center, center, radius + 1.25, RGB(24, 26, 32));
  Disc(pixels, size, center, center, radius, color);
  return IconFromPixels(pixels, size);
}

}  // namespace sovereign::tray
