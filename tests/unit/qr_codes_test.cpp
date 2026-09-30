// QR codes read from pictures (src/tray/qr_codes.h): codes drawn here with
// ZXing's own writer the way a screen shows them - small among other pixels,
// several at once, light on dark, rows padded - and read back.

#include <ZXing/BitMatrix.h>
#include <ZXing/CharacterSet.h>
#include <ZXing/MultiFormatWriter.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "check.h"
#include "qr_codes.h"
#include "share_links.h"

namespace {

using sovereign::tray::BgraImage;
using sovereign::tray::ReadQrCodes;

// A canvas: BGRA, a grey page with some noise, rows `pad` bytes longer than needed.
struct Canvas {
  int width;
  int height;
  int stride;
  std::vector<std::uint8_t> pixels;

  Canvas(int w, int h, int pad) : width(w), height(h), stride(w * 4 + pad), pixels(static_cast<std::size_t>(stride) * h) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const auto grey = static_cast<std::uint8_t>(200 + ((x * 7 + y * 13) % 40));
        Set(x, y, grey);
      }
    }
  }

  void Set(int x, int y, std::uint8_t value) {
    std::uint8_t* pixel = &pixels[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4];
    pixel[0] = value;
    pixel[1] = value;
    pixel[2] = value;
    pixel[3] = 255;
  }

  // The code for `text` at (left, top), `scale` pixels a module, with its quiet zone.
  void Draw(const std::string& text, int left, int top, int scale, bool inverted = false) {
    const ZXing::BitMatrix code =
        ZXing::MultiFormatWriter(ZXing::BarcodeFormat::QRCode).setEncoding(ZXing::CharacterSet::UTF8).setMargin(4).encode(text, 0, 0);
    const std::uint8_t dark = inverted ? 240 : 10;
    const std::uint8_t light = inverted ? 20 : 250;
    for (int y = 0; y < code.height() * scale; ++y) {
      for (int x = 0; x < code.width() * scale; ++x) {
        if (left + x < width && top + y < height) {
          Set(left + x, top + y, code.get(x / scale, y / scale) ? dark : light);
        }
      }
    }
  }

  BgraImage Image() const { return {pixels.data(), width, height, stride}; }
};

std::vector<std::string> Sorted(std::vector<std::string> texts) {
  std::sort(texts.begin(), texts.end());
  return texts;
}

const std::string kVless =
    "vless://11111111-2222-3333-4444-555555555555@nl.example.com:443?security=reality&sni=www.microsoft.com"
    "&fp=chrome&pbk=AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE&sid=6ba85179e30d4fc2&flow=xtls-rprx-vision"
    "#\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1 Нидерланды";
const std::string kSub = "https://sub.example.com/api/v1/client/subscribe?token=0123456789abcdef";

void TestOne() {
  Canvas canvas(800, 600, 0);
  canvas.Draw(kVless, 300, 150, 3);
  const auto texts = ReadQrCodes(canvas.Image());
  CHECK(texts == std::vector<std::string>({kVless}));
  // What it holds is a key the import takes.
  const auto items = sovereign::tray::RecognizeImport(texts.empty() ? std::string() : texts.front());
  CHECK(items.links.size() == 1);
  CHECK(!items.links.empty() && sovereign::tray::ParseShareLink(items.links[0]).name ==
                                    "\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1 Нидерланды");
}

void TestSeveral() {
  // Two codes on a wide screen, rows padded like a DIB section's may be.
  Canvas canvas(1920, 1080, 12);
  canvas.Draw(kVless, 100, 200, 4);
  canvas.Draw(kSub, 1300, 500, 5);
  CHECK(Sorted(ReadQrCodes(canvas.Image())) == Sorted({kVless, kSub}));
}

void TestInverted() {
  Canvas canvas(600, 600, 0);
  canvas.Draw(kSub, 100, 100, 5, /*inverted=*/true);  // a dark theme's code
  CHECK(ReadQrCodes(canvas.Image()) == std::vector<std::string>({kSub}));
}

void TestNothing() {
  Canvas canvas(640, 480, 0);
  CHECK(ReadQrCodes(canvas.Image()).empty());
  CHECK(ReadQrCodes({}).empty());
  CHECK(ReadQrCodes({canvas.pixels.data(), 640, 480, 100}).empty());  // a stride shorter than a row
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestOne();
    TestSeveral();
    TestInverted();
    TestNothing();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
