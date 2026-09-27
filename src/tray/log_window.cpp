#include "log_window.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include <wil/resource.h>

#include <string>

#include "icons.h"

namespace sovereign::tray {

namespace {

constexpr wchar_t kClassName[] = L"SovereignLogWindow";
constexpr int kWidthDip = 920;
constexpr int kHeightDip = 540;
// Past this many lines the oldest go (while following the bottom): an edit
// control slows down with megabytes of text, and the tray keeps no more anyway.
constexpr int kMaxLines = 6000;
constexpr int kTrimTo = 5000;
constexpr COLORREF kBackground = RGB(28, 31, 38);
constexpr COLORREF kText = RGB(222, 226, 234);

int Scale(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }

std::wstring Joined(const std::vector<std::wstring>& lines) {
  std::wstring text;
  for (const std::wstring& line : lines) {
    text += line;
    text += L"\r\n";
  }
  return text;
}

}  // namespace

struct LogWindow::Impl {
  HINSTANCE instance;
  HWND window = nullptr;
  HWND edit = nullptr;
  wil::unique_hfont font;
  wil::unique_hbrush background{CreateSolidBrush(kBackground)};
  wil::unique_hicon bigIcon;
  wil::unique_hicon smallIcon;

  explicit Impl(HINSTANCE inst) : instance(inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &Impl::WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW, see flyout.cpp
    wc.hbrBackground = background.get();
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);
  }

  ~Impl() {
    if (window != nullptr) {
      DestroyWindow(window);
    }
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
      // The creation parameter arrives as an LPARAM - the cast is the API's shape.
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);  // NOLINT(performance-no-int-to-ptr)
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));  // NOLINT(performance-no-int-to-ptr)
    return self != nullptr ? self->Handle(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
  }

  void ApplyFont(UINT dpi) {
    font.reset(CreateFontW(-MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           FIXED_PITCH | FF_MODERN, L"Consolas"));
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font.get()), TRUE);
    const int margin = Scale(8, dpi);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(margin, margin));
  }

  LRESULT Handle(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
      case WM_CREATE:
        edit = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                   ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                               0, 0, 0, 0, hwnd, nullptr, instance, nullptr);
        if (edit == nullptr) {
          return -1;
        }
        SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);  // multiline: as much as memory allows
        SetWindowTheme(edit, L"DarkMode_Explorer", nullptr);  // dark scroll bars
        ApplyFont(GetDpiForWindow(hwnd));
        return 0;
      case WM_SIZE:
        MoveWindow(edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        return 0;
      case WM_SETFOCUS:
        SetFocus(edit);
        return 0;
      case WM_CTLCOLORSTATIC:  // what a read-only edit asks for its colors
        SetTextColor(reinterpret_cast<HDC>(wParam), kText);  // NOLINT(performance-no-int-to-ptr)
        SetBkColor(reinterpret_cast<HDC>(wParam), kBackground);  // NOLINT(performance-no-int-to-ptr)
        return reinterpret_cast<LRESULT>(background.get());
      case WM_DPICHANGED: {
        // The system's suggestion keeps the window the same physical size.
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);  // NOLINT(performance-no-int-to-ptr)
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyFont(HIWORD(wParam));
        return 0;
      }
      case WM_NCDESTROY:
        window = nullptr;
        edit = nullptr;
        return DefWindowProcW(hwnd, message, wParam, lParam);
      default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
  }

  bool Create() {
    window = CreateWindowExW(0, kClassName, L"Sovereign — журнал", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, this);
    if (window == nullptr) {
      return false;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    const UINT dpi = GetDpiForWindow(window);
    bigIcon = LoadAppIcon(GetSystemMetricsForDpi(SM_CXICON, dpi));
    smallIcon = LoadAppIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi));
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon.get()));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon.get()));
    SetWindowPos(window, nullptr, 0, 0, Scale(kWidthDip, dpi), Scale(kHeightDip, dpi),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
  }

  // Whether the view shows the last line (or there is nothing to scroll).
  bool AtBottom() const {
    SCROLLINFO si{};
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL;
    if (!GetScrollInfo(edit, SB_VERT, &si)) {
      return true;
    }
    return si.nPos + static_cast<int>(si.nPage) > si.nMax;
  }

  void Replace(DWORD from, DWORD to, const std::wstring& text) {
    SendMessageW(edit, EM_SETSEL, from, to);
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
  }

  void Append(const std::wstring& text) {
    const bool follow = AtBottom();
    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
    const auto firstVisible = static_cast<int>(SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0));

    SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
    const auto length = static_cast<DWORD>(GetWindowTextLengthW(edit));
    Replace(length, length, text);
    if (follow) {
      const auto lines = static_cast<int>(SendMessageW(edit, EM_GETLINECOUNT, 0, 0));
      if (lines > kMaxLines) {
        const auto cut = static_cast<DWORD>(SendMessageW(edit, EM_LINEINDEX, lines - kTrimTo, 0));
        Replace(0, cut, L"");
        selStart = selStart > cut ? selStart - cut : 0;
        selEnd = selEnd > cut ? selEnd - cut : 0;
      }
      SendMessageW(edit, EM_SETSEL, selStart, selEnd);
      SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
    } else {
      SendMessageW(edit, EM_SETSEL, selStart, selEnd);
      const auto now = static_cast<int>(SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0));
      SendMessageW(edit, EM_LINESCROLL, 0, firstVisible - now);
    }
    SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(edit, nullptr, TRUE);
  }
};

LogWindow::LogWindow(HINSTANCE instance) : impl_(std::make_unique<Impl>(instance)) {}

LogWindow::~LogWindow() = default;

void LogWindow::Show(const std::vector<std::wstring>& lines) {
  if (impl_->window == nullptr && !impl_->Create()) {
    return;
  }
  SetWindowTextW(impl_->edit, Joined(lines).c_str());
  SendMessageW(impl_->edit, WM_VSCROLL, SB_BOTTOM, 0);
  ShowWindow(impl_->window, IsIconic(impl_->window) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(impl_->window);
}

bool LogWindow::IsOpen() const { return impl_->window != nullptr; }

void LogWindow::Append(const std::vector<std::wstring>& lines) {
  if (impl_->window != nullptr && !lines.empty()) {
    impl_->Append(Joined(lines));
  }
}

}  // namespace sovereign::tray
