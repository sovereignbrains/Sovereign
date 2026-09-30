#include "capture.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string_view>

#include "qr_codes.h"

namespace sovereign::tray {

namespace {

// Pictures bigger than this aren't read: three 8K screens side by side.
constexpr std::uint64_t kMaxPixels = 110'000'000;

// Files past these aren't looked at: nobody copies more to add a subscription.
constexpr UINT kMaxClipboardFiles = 16;

struct Pixels {
  std::vector<std::uint8_t> data;  // BGRA, top row first, no padding
  int width = 0;
  int height = 0;
};

std::vector<std::string> Read(const std::optional<Pixels>& pixels) {
  if (!pixels) {
    return {};
  }
  return ReadQrCodes({pixels->data.data(), pixels->width, pixels->height, pixels->width * 4});
}

// A GDI bitmap's pixels (not selected into a DC).
std::optional<Pixels> FromBitmap(HBITMAP bitmap) {
  BITMAP info{};
  if (GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight == 0) {
    return std::nullopt;
  }
  const int width = info.bmWidth;
  const int height = std::abs(info.bmHeight);
  if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxPixels) {
    return std::nullopt;
  }
  BITMAPINFO header{};
  header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  header.bmiHeader.biWidth = width;
  header.bmiHeader.biHeight = -height;  // top row first
  header.bmiHeader.biPlanes = 1;
  header.bmiHeader.biBitCount = 32;
  header.bmiHeader.biCompression = BI_RGB;
  Pixels pixels;
  pixels.width = width;
  pixels.height = height;
  pixels.data.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  const auto screen = wil::GetDC(nullptr);
  if (!screen || GetDIBits(screen.get(), bitmap, 0, static_cast<UINT>(height), pixels.data.data(), &header,
                           DIB_RGB_COLORS) != height) {
    return std::nullopt;
  }
  return pixels;
}

// A WIC picture over white paper: a QR code with a transparent background
// would otherwise have black (premultiplied zero) between its modules.
std::optional<Pixels> FromWic(IWICBitmapSource* source) {
  wil::com_ptr<IWICBitmapSource> converted;
  if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, source, &converted))) {
    return std::nullopt;
  }
  UINT width = 0;
  UINT height = 0;
  if (FAILED(converted->GetSize(&width, &height)) || width == 0 || height == 0 ||
      static_cast<std::uint64_t>(width) * height > kMaxPixels) {
    return std::nullopt;
  }
  Pixels pixels;
  pixels.width = static_cast<int>(width);
  pixels.height = static_cast<int>(height);
  pixels.data.resize(static_cast<std::size_t>(width) * height * 4);
  if (FAILED(converted->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.data.size()), pixels.data.data()))) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i + 3 < pixels.data.size(); i += 4) {
    const int paper = 255 - pixels.data[i + 3];
    for (std::size_t c = 0; c < 3; ++c) {
      pixels.data[i + c] = static_cast<std::uint8_t>(std::min(255, pixels.data[i + c] + paper));
    }
    pixels.data[i + 3] = 255;
  }
  return pixels;
}

std::optional<Pixels> FromDecoder(IWICBitmapDecoder* decoder) {
  wil::com_ptr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) {
    return std::nullopt;
  }
  return FromWic(frame.get());
}

wil::com_ptr<IWICImagingFactory> Wic() {
  return wil::CoCreateInstanceNoThrow<IWICImagingFactory>(CLSID_WICImagingFactory);
}

// The "PNG" format browsers and messengers put on the clipboard next to the
// bitmap: unlike the bitmap, it keeps transparency. The clipboard is open.
std::optional<Pixels> FromClipboardPng() {
  const UINT format = RegisterClipboardFormatW(L"PNG");
  HANDLE data = format != 0 ? GetClipboardData(format) : nullptr;
  const SIZE_T size = data != nullptr ? GlobalSize(data) : 0;
  if (size == 0 || size > UINT_MAX) {
    return std::nullopt;
  }
  const auto factory = Wic();
  const void* locked = GlobalLock(data);
  if (!factory || locked == nullptr) {
    return std::nullopt;
  }
  wil::com_ptr<IStream> stream;
  stream.attach(SHCreateMemStream(static_cast<const BYTE*>(locked), static_cast<UINT>(size)));  // a copy
  GlobalUnlock(data);
  wil::com_ptr<IWICBitmapDecoder> decoder;
  if (!stream ||
      FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
    return std::nullopt;
  }
  return FromDecoder(decoder.get());
}

std::wstring Lower(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return text;
}

}  // namespace

std::vector<std::string> ClipboardQrCodes(HWND owner) {
  std::optional<Pixels> pixels;
  {
    if (!OpenClipboard(owner)) {
      return {};
    }
    const auto close = wil::scope_exit([] { CloseClipboard(); });
    pixels = FromClipboardPng();
    if (!pixels && IsClipboardFormatAvailable(CF_BITMAP)) {
      if (auto* bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP))) {
        pixels = FromBitmap(bitmap);
      }
    }
  }
  return Read(pixels);  // with the clipboard closed: reading takes a moment
}

std::vector<std::wstring> ClipboardFiles(HWND owner) {
  std::vector<std::wstring> paths;
  if (!OpenClipboard(owner)) {
    return paths;
  }
  const auto close = wil::scope_exit([] { CloseClipboard(); });
  auto* drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
  if (drop == nullptr) {
    return paths;
  }
  const UINT count = std::min(DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0), kMaxClipboardFiles);
  for (UINT i = 0; i < count; ++i) {
    const UINT length = DragQueryFileW(drop, i, nullptr, 0);
    std::wstring path(length + 1, L'\0');
    if (length > 0 && DragQueryFileW(drop, i, path.data(), length + 1) == length) {
      path.resize(length);
      paths.push_back(std::move(path));
    }
  }
  return paths;
}

bool IsImageFile(const std::wstring& path) {
  static constexpr std::array<std::wstring_view, 16> kImages = {
      L".png", L".jpg", L".jpeg", L".jfif", L".bmp",  L".dib",  L".gif", L".tif",
      L".tiff", L".webp", L".ico", L".heic", L".heif", L".avif", L".jxr", L".wdp"};
  const std::wstring extension = Lower(std::filesystem::path(path).extension().wstring());
  return std::find(kImages.begin(), kImages.end(), extension) != kImages.end();
}

std::vector<std::string> ImageFileQrCodes(const std::wstring& path) {
  const auto factory = Wic();
  wil::com_ptr<IWICBitmapDecoder> decoder;
  if (!factory || FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnDemand, &decoder))) {
    return {};
  }
  return Read(FromDecoder(decoder.get()));
}

std::vector<std::string> ScreenQrCodes() {
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (width <= 0 || height <= 0) {
    return {};
  }
  const auto screen = wil::GetDC(nullptr);
  if (!screen) {
    return {};
  }
  wil::unique_hdc memory(CreateCompatibleDC(screen.get()));
  wil::unique_hbitmap bitmap(CreateCompatibleBitmap(screen.get(), width, height));
  if (!memory || !bitmap) {
    return {};
  }
  {
    const auto selected = wil::SelectObject(memory.get(), bitmap.get());
    if (!BitBlt(memory.get(), 0, 0, width, height, screen.get(), left, top, SRCCOPY | CAPTUREBLT)) {
      return {};
    }
  }
  return Read(FromBitmap(bitmap.get()));
}

}  // namespace sovereign::tray
