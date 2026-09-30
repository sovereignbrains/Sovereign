// Where the tray finds pictures of QR codes (src/tray/capture.h): an image
// file (a PNG with a transparent background - black paper otherwise), the
// clipboard (a bitmap, the "PNG" format browsers add, files copied in
// Explorer) and the screen (a window showing a code).
//
// The clipboard and screen checks need a desktop and change the clipboard
// (its text is put back): they run in CI (CI is set on GitHub's runners) or
// with SOVEREIGN_DESKTOP_TESTS=1, and say they're skipped otherwise.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <ZXing/BitMatrix.h>
#include <ZXing/CharacterSet.h>
#include <ZXing/MultiFormatWriter.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "check.h"

namespace {

using namespace sovereign::tray;

struct Picture {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> bgra;  // straight alpha
};

// `text`'s code, `scale` pixels a module: black modules on white, or on nothing.
Picture Qr(const std::string& text, int scale, bool transparent) {
  const ZXing::BitMatrix code =
      ZXing::MultiFormatWriter(ZXing::BarcodeFormat::QRCode).setEncoding(ZXing::CharacterSet::UTF8).setMargin(4).encode(text, 0, 0);
  Picture picture;
  picture.width = code.width() * scale;
  picture.height = code.height() * scale;
  picture.bgra.resize(static_cast<std::size_t>(picture.width) * static_cast<std::size_t>(picture.height) * 4);
  for (int y = 0; y < picture.height; ++y) {
    for (int x = 0; x < picture.width; ++x) {
      std::uint8_t* pixel = &picture.bgra[(static_cast<std::size_t>(y) * static_cast<std::size_t>(picture.width) +
                                           static_cast<std::size_t>(x)) * 4];
      const bool dark = code.get(x / scale, y / scale);
      const std::uint8_t value = dark ? 0 : 255;
      pixel[0] = transparent && !dark ? 0 : value;
      pixel[1] = transparent && !dark ? 0 : value;
      pixel[2] = transparent && !dark ? 0 : value;
      pixel[3] = transparent && !dark ? 0 : 255;
    }
  }
  return picture;
}

BITMAPINFO TopDown(const Picture& picture) {
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = picture.width;
  info.bmiHeader.biHeight = -picture.height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  return info;
}

// The picture as a PNG, written by WIC into `stream`.
bool EncodePng(const Picture& picture, IStream* stream) {
  const auto factory = wil::CoCreateInstanceNoThrow<IWICImagingFactory>(CLSID_WICImagingFactory);
  wil::com_ptr<IWICBitmapEncoder> encoder;
  wil::com_ptr<IWICBitmapFrameEncode> frame;
  if (!factory || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
      FAILED(frame->SetSize(static_cast<UINT>(picture.width), static_cast<UINT>(picture.height)))) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA) {
    return false;
  }
  auto* pixels = const_cast<BYTE*>(picture.bgra.data());  // WritePixels doesn't write to them
  return SUCCEEDED(frame->WritePixels(static_cast<UINT>(picture.height), static_cast<UINT>(picture.width) * 4,
                                      static_cast<UINT>(picture.bgra.size()), pixels)) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

const std::string kFileCode = "trojan://pw@file.example.com:443#From a file";
const std::string kBitmapCode = "vless://11111111-2222-3333-4444-555555555555@clip.example.com:443?security=tls#Clip";
const std::string kPngCode = "https://sub.example.com/png-on-the-clipboard";
const std::string kScreenCode = "hy2://pw@screen.example.com:443#On the screen";

std::filesystem::path TempFile(const wchar_t* name) {
  return std::filesystem::temp_directory_path() / name;
}

void TestImageFile() {
  const auto path = TempFile(L"sovereign-capture-test.png");
  {
    wil::com_ptr<IStream> stream;
    CHECK(SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE,
                                           nullptr, &stream)));
    CHECK(stream && EncodePng(Qr(kFileCode, 3, /*transparent=*/true), stream.get()));
  }
  CHECK(IsImageFile(path.wstring()));
  CHECK(IsImageFile(L"C:\\x\\QR.JPG"));
  CHECK(!IsImageFile(L"C:\\x\\config.json"));
  CHECK(ImageFileQrCodes(path.wstring()) == std::vector<std::string>({kFileCode}));
  CHECK(ImageFileQrCodes(TempFile(L"sovereign-no-such-file.png").wstring()).empty());
  std::filesystem::remove(path);
}

// The clipboard's text, to put back.
std::optional<std::wstring> SaveClipboardText(HWND owner) {
  if (!OpenClipboard(owner)) {
    return std::nullopt;
  }
  std::optional<std::wstring> text;
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* locked = static_cast<const wchar_t*>(GlobalLock(data))) {
      text = locked;
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return text;
}

// The clipboard emptied and given `data` in `format`; it owns the data then.
bool Put(HWND owner, UINT format, HANDLE data) {
  if (!OpenClipboard(owner)) {
    return false;
  }
  const bool put = EmptyClipboard() && SetClipboardData(format, data) != nullptr;
  CloseClipboard();
  return put;
}

wil::unique_hglobal Global(const void* bytes, std::size_t size) {
  wil::unique_hglobal memory(GlobalAlloc(GMEM_MOVEABLE, size));
  if (memory) {
    if (void* locked = GlobalLock(memory.get())) {
      std::memcpy(locked, bytes, size);
      GlobalUnlock(memory.get());
    }
  }
  return memory;
}

void TestClipboard(HWND owner) {
  const auto saved = SaveClipboardText(owner);

  // A bitmap, as a screenshot tool puts it.
  const Picture opaque = Qr(kBitmapCode, 3, /*transparent=*/false);
  const BITMAPINFO info = TopDown(opaque);
  void* bits = nullptr;
  wil::unique_hbitmap bitmap(CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0));
  CHECK(bitmap && bits != nullptr);
  if (bits != nullptr) {
    std::memcpy(bits, opaque.bgra.data(), opaque.bgra.size());
  }
  if (Put(owner, CF_BITMAP, bitmap.get())) {
    bitmap.release();  // the clipboard's now
  }
  CHECK(ClipboardQrCodes(owner) == std::vector<std::string>({kBitmapCode}));

  // Only the "PNG" format, the code on a transparent background.
  wil::com_ptr<IStream> stream;
  CHECK(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)));
  CHECK(EncodePng(Qr(kPngCode, 3, /*transparent=*/true), stream.get()));
  HGLOBAL encoded = nullptr;
  CHECK(SUCCEEDED(GetHGlobalFromStream(stream.get(), &encoded)));
  STATSTG stat{};
  CHECK(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)));
  const void* bytes = encoded != nullptr ? GlobalLock(encoded) : nullptr;
  CHECK(bytes != nullptr);
  auto png = bytes != nullptr ? Global(bytes, static_cast<std::size_t>(stat.cbSize.QuadPart)) : wil::unique_hglobal();
  if (bytes != nullptr) {
    GlobalUnlock(encoded);
  }
  if (Put(owner, RegisterClipboardFormatW(L"PNG"), png.get())) {
    png.release();
  }
  CHECK(ClipboardQrCodes(owner) == std::vector<std::string>({kPngCode}));

  // Files copied in Explorer.
  const std::wstring path = TempFile(L"sovereign-keys.txt").wstring();
  std::vector<std::uint8_t> drop(sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t), 0);
  DROPFILES header{};
  header.pFiles = sizeof(DROPFILES);
  header.fWide = TRUE;
  std::memcpy(drop.data(), &header, sizeof(header));
  std::memcpy(drop.data() + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
  auto files = Global(drop.data(), drop.size());
  if (Put(owner, CF_HDROP, files.get())) {
    files.release();
  }
  CHECK(ClipboardFiles(owner) == std::vector<std::wstring>({path}));
  CHECK(ClipboardQrCodes(owner).empty());  // files, no picture

  if (saved) {
    auto text = Global(saved->c_str(), (saved->size() + 1) * sizeof(wchar_t));
    if (Put(owner, CF_UNICODETEXT, text.get())) {
      text.release();
    }
  } else if (OpenClipboard(owner)) {
    EmptyClipboard();
    CloseClipboard();
  }
}

const Picture* g_shown = nullptr;

LRESULT CALLBACK ShowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_PAINT && g_shown != nullptr) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    const BITMAPINFO info = TopDown(*g_shown);
    StretchDIBits(dc, 0, 0, g_shown->width, g_shown->height, 0, 0, g_shown->width, g_shown->height,
                  g_shown->bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    EndPaint(window, &paint);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void Pump(std::chrono::milliseconds duration) {
  const auto until = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < until) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void TestScreen(HINSTANCE instance) {
  const Picture picture = Qr(kScreenCode, 4, /*transparent=*/false);
  g_shown = &picture;
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = ShowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = L"SovereignCaptureTest";
  RegisterClassW(&windowClass);
  wil::unique_hwnd window(CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass.lpszClassName,
                                          L"", WS_POPUP, 40, 40, picture.width, picture.height, nullptr, nullptr,
                                          instance, nullptr));
  CHECK(static_cast<bool>(window));
  ShowWindow(window.get(), SW_SHOWNOACTIVATE);
  UpdateWindow(window.get());
  Pump(std::chrono::milliseconds(600));  // until the compositor shows it
  const auto codes = ScreenQrCodes();
  CHECK(std::find(codes.begin(), codes.end(), kScreenCode) != codes.end());
  window.reset();
  g_shown = nullptr;
}

bool DesktopTests() {
  for (const wchar_t* name : {L"CI", L"SOVEREIGN_DESKTOP_TESTS"}) {
    std::array<wchar_t, 8> value{};
    if (GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size())) > 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // as the tray: real pixels
    const auto com = wil::CoInitializeEx(COINIT_APARTMENTTHREADED);
    TestImageFile();
    if (DesktopTests()) {
      const HINSTANCE instance = GetModuleHandleW(nullptr);
      wil::unique_hwnd owner(CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr));
      TestClipboard(owner.get());
      TestScreen(instance);
    } else {
      std::cout << "clipboard and screen: skipped (set SOVEREIGN_DESKTOP_TESTS=1; CI runs them)\n";
    }
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
