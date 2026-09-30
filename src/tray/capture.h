#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

// Pictures the tray reads QR codes from (qr_codes.h): what the clipboard
// holds besides text, image files, the screen. Needs COM on the calling
// thread (WIC); the tray's UI thread has it.

namespace sovereign::tray {

// A picture on the clipboard: a screenshot, an image copied from a browser
// or a messenger. The texts of its QR codes; none when there's no picture.
std::vector<std::string> ClipboardQrCodes(HWND owner);

// Files copied in Explorer.
std::vector<std::wstring> ClipboardFiles(HWND owner);

// png, jpg, bmp, gif, tiff, webp, ico, heic... - whatever WIC decodes.
bool IsImageFile(const std::wstring& path);
std::vector<std::string> ImageFileQrCodes(const std::wstring& path);

// Every monitor, as it is on the screen now.
std::vector<std::string> ScreenQrCodes();

}  // namespace sovereign::tray
