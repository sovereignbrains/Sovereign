#include "qr_codes.h"

#include <ZXing/ReadBarcode.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

namespace sovereign::tray {

namespace {

void Collect(const ZXing::ImageView& view, const ZXing::ReaderOptions& options, std::vector<std::string>& texts) {
  for (const ZXing::Barcode& barcode : ZXing::ReadBarcodes(view, options)) {
    std::string text = barcode.text();
    if (barcode.isValid() && !text.empty() && std::find(texts.begin(), texts.end(), text) == texts.end()) {
      texts.push_back(std::move(text));
    }
  }
}

// A byte a pixel, top row first, no padding.
std::vector<std::uint8_t> Luminance(const BgraImage& image) {
  std::vector<std::uint8_t> lum(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));
  for (int y = 0; y < image.height; ++y) {
    const std::uint8_t* pixel = image.pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.stride);
    std::uint8_t* out = lum.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width);
    for (int x = 0; x < image.width; ++x, pixel += 4) {
      out[x] = static_cast<std::uint8_t>((pixel[0] * 29 + pixel[1] * 150 + pixel[2] * 77) >> 8);  // B, G, R
    }
  }
  return lum;
}

// Each pixel the lightest (`pick` std::max) or the darkest of a square
// `radius` pixels around it: by rows, then by columns.
template <typename Pick>
void Spread(std::vector<std::uint8_t>& lum, int width, int height, int radius, Pick pick) {
  std::vector<std::uint8_t> row(static_cast<std::size_t>(width));
  for (int y = 0; y < height; ++y) {
    std::uint8_t* line = lum.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
    std::copy(line, line + width, row.begin());
    for (int x = 0; x < width; ++x) {
      std::uint8_t value = row[static_cast<std::size_t>(x)];
      for (int i = std::max(0, x - radius); i <= std::min(width - 1, x + radius); ++i) {
        value = pick(value, row[static_cast<std::size_t>(i)]);
      }
      line[x] = value;
    }
  }
  const std::vector<std::uint8_t> rows = lum;
  for (int y = 0; y < height; ++y) {
    std::uint8_t* line = lum.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
    for (int i = std::max(0, y - radius); i <= std::min(height - 1, y + radius); ++i) {
      const std::uint8_t* other = rows.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(width);
      for (int x = 0; x < width; ++x) {
        line[x] = pick(line[x], other[x]);
      }
    }
  }
}

// The picture without its lines at most 2 * radius pixels thick: dark ones
// across light (a closing) - the gaps a terminal leaves between its rows of
// blocks, as `qrencode -t UTF8` draws a code - or light ones across dark (an
// opening: a light theme's terminal). What is wider than the lines, a code's
// modules among it, keeps its shape.
std::vector<std::uint8_t> WithoutLines(std::vector<std::uint8_t> lum, int width, int height, int radius,
                                       bool darkLines) {
  const auto lighter = [](std::uint8_t a, std::uint8_t b) { return std::max(a, b); };
  const auto darker = [](std::uint8_t a, std::uint8_t b) { return std::min(a, b); };
  if (darkLines) {
    Spread(lum, width, height, radius, lighter);
    Spread(lum, width, height, radius, darker);
  } else {
    Spread(lum, width, height, radius, darker);
    Spread(lum, width, height, radius, lighter);
  }
  return lum;
}

}  // namespace

std::vector<std::string> ReadQrCodes(const BgraImage& image) {
  std::vector<std::string> texts;
  if (image.pixels == nullptr || image.width <= 0 || image.height <= 0 || image.stride < image.width * 4) {
    return texts;
  }
  ZXing::ReaderOptions options;
  options.setFormats(ZXing::BarcodeFormat::QRCode | ZXing::BarcodeFormat::MicroQRCode);
  // A screenshot has the code small, maybe light on dark (a dark theme),
  // maybe several of them.
  options.setTryHarder(true);
  options.setTryRotate(true);
  options.setTryInvert(true);
  options.setTryDownscale(true);
  options.setTextMode(ZXing::TextMode::Plain);
  try {
    Collect(ZXing::ImageView(image.pixels, image.width, image.height, ZXing::ImageFormat::BGRA, image.stride), options,
            texts);
    if (!texts.empty()) {
      return texts;
    }
    // None: maybe a code drawn with characters in a terminal, its rows of
    // blocks apart - a line between every two rows of modules breaks the
    // finder patterns (packetlab's in the user's terminal, 28.10.2026: read
    // at 100% and 250%, not between). Read again without such lines, the
    // thinnest first: up to 4 pixels, a 200% screen's (a 4K screen with no
    // code: under half a second more).
    const std::vector<std::uint8_t> lum = Luminance(image);
    for (const bool darkLines : {true, false}) {
      for (const int radius : {1, 2}) {
        const std::vector<std::uint8_t> clean = WithoutLines(lum, image.width, image.height, radius, darkLines);
        Collect(ZXing::ImageView(clean.data(), image.width, image.height, ZXing::ImageFormat::Lum), options, texts);
        if (!texts.empty()) {
          return texts;
        }
      }
    }
  } catch (const std::exception&) {
    // ZXing throws on what it can't take (an image too big for its sizes):
    // no code read, the caller says so.
    texts.clear();
  }
  return texts;
}

}  // namespace sovereign::tray
