#pragma once

#include <cstdint>
#include <string>
#include <vector>

// QR codes in a picture - a screenshot on the clipboard, an image file, the
// screen itself - read with ZXing. What they hold goes where pasted text goes
// (share_links.h). No Win32 here: the pixels come from main.cpp's capture;
// unit-tested in tests/unit/qr_codes_test.cpp.

namespace sovereign::tray {

// 32-bit pixels, blue-green-red-unused in memory (a Windows DIB), top row
// first; `stride` bytes from one row to the next.
struct BgraImage {
  const std::uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
};

// The text of every QR code in the picture, each once, in reading order.
std::vector<std::string> ReadQrCodes(const BgraImage& image);

}  // namespace sovereign::tray
