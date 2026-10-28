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
  void Draw(std::string_view text, int left, int top, int scale, bool inverted = false) {
    // Newer ZXing marks MultiFormatWriter deprecated (CreateBarcode), an older one
    // (the build job's cache) has no CreateBarcode: the warning is silenced, not obeyed.
#pragma warning(push)
#pragma warning(disable : 4996)
    const ZXing::BitMatrix code =
        ZXing::MultiFormatWriter(ZXing::BarcodeFormat::QRCode).setEncoding(ZXing::CharacterSet::UTF8).setMargin(4).encode(std::string(text), 0, 0);
#pragma warning(pop)
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

  void Fill(std::uint8_t value) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        Set(x, y, value);
      }
    }
  }

  // The code as `qrencode -t UTF8 -m 2` prints it, in a terminal of the
  // canvas's colour: its light modules are blocks of `block`, a character two
  // modules tall, and between the rows of characters `gap` pixels of the
  // terminal itself.
  void DrawTerminal(std::string_view text, int left, int top, int module, int gap, std::uint8_t block) {
#pragma warning(push)
#pragma warning(disable : 4996)
    const ZXing::BitMatrix code =
        ZXing::MultiFormatWriter(ZXing::BarcodeFormat::QRCode).setEncoding(ZXing::CharacterSet::UTF8).setMargin(2).encode(std::string(text), 0, 0);
#pragma warning(pop)
    for (int row = 0; row < code.height(); ++row) {
      const int y0 = top + (row / 2) * (2 * module + gap) + (row % 2) * module;
      for (int column = 0; column < code.width(); ++column) {
        for (int y = y0; y < y0 + module && y < height; ++y) {
          for (int x = left + column * module; x < left + (column + 1) * module && x < width; ++x) {
            if (!code.get(column, row)) {
              Set(x, y, block);
            }
          }
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

constexpr std::string_view kVless =
    "vless://11111111-2222-3333-4444-555555555555@nl.example.com:443?security=reality&sni=www.microsoft.com"
    "&fp=chrome&pbk=AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE&sid=6ba85179e30d4fc2&flow=xtls-rprx-vision"
    "#\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1 Нидерланды";
constexpr std::string_view kSub = "https://sub.example.com/api/v1/client/subscribe?token=0123456789abcdef";

void TestOne() {
  Canvas canvas(800, 600, 0);
  canvas.Draw(kVless, 300, 150, 3);
  const auto texts = ReadQrCodes(canvas.Image());
  CHECK(texts == std::vector<std::string>({std::string(kVless)}));
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
  CHECK(Sorted(ReadQrCodes(canvas.Image())) == Sorted({std::string(kVless), std::string(kSub)}));
}

void TestInverted() {
  Canvas canvas(600, 600, 0);
  canvas.Draw(kSub, 100, 100, 5, /*inverted=*/true);  // a dark theme's code
  CHECK(ReadQrCodes(canvas.Image()) == std::vector<std::string>({std::string(kSub)}));
}

// packetlab's "QR для приложения sing-box" in a terminal (28.10.2026: not
// read from the screen or the clipboard): 7-pixel modules, 2 pixels between
// the rows of characters - in a dark terminal and in a light one.
void TestTerminal() {
  const std::string link =
      "sing-box://import-remote-profile?url="
      "https%3A%2F%2Fsub.example.com%2Fapi%2Fv1%2Fclient%2Fsubscribe%3Ftoken%3D0123456789abcdef#packetlab";
  for (const bool light : {false, true}) {
    for (const int gap : {1, 2, 3}) {
      Canvas canvas(900, 700, 0);
      canvas.Fill(light ? 245 : 25);
      canvas.DrawTerminal(link, 60, 40, 7, gap, light ? 40 : 180);
      CHECK(ReadQrCodes(canvas.Image()) == std::vector<std::string>({link}));
    }
  }
  // What it holds is the subscription the import takes.
  CHECK(sovereign::tray::RecognizeImport(link).urls == std::vector<std::string>({std::string(kSub)}));
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
    TestTerminal();
    TestNothing();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
