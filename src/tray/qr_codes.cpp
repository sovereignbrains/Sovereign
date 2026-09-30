#include "qr_codes.h"

#include <ZXing/ReadBarcode.h>

#include <algorithm>
#include <exception>

namespace sovereign::tray {

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
    const ZXing::ImageView view(image.pixels, image.width, image.height, ZXing::ImageFormat::BGRA, image.stride);
    for (const ZXing::Barcode& barcode : ZXing::ReadBarcodes(view, options)) {
      std::string text = barcode.text();
      if (barcode.isValid() && !text.empty() && std::find(texts.begin(), texts.end(), text) == texts.end()) {
        texts.push_back(std::move(text));
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
