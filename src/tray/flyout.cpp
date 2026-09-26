#include "flyout.h"

#include <windowsx.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellscalingapi.h>

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>

namespace sovereign::tray {

namespace {

constexpr wchar_t kClassName[] = L"SovereignTrayFlyout";

// Layout, in DIPs (1/96 inch): the render target works in DIPs at the
// monitor's DPI, so nothing below needs scaling by hand.
constexpr float kWidth = 340;
constexpr float kPad = 12;
constexpr float kHeader = 58;
constexpr float kRow = 40;
constexpr float kFooter = 38;
constexpr float kGap = 6;
constexpr float kRowsTop = kPad + kHeader + kGap;
constexpr float kFooterTop = kRowsTop + 3 * kRow + kGap;
constexpr float kHeight = kFooterTop + kFooter + kPad;
constexpr float kButton = 28;  // the small icon buttons in the subscription row

// Segoe Fluent Icons (Windows 11; the same code points in Segoe MDL2 Assets).
constexpr wchar_t kGlyphPower[] = L"\xE7E8";
constexpr wchar_t kGlyphSync[] = L"\xE895";
constexpr wchar_t kGlyphPaste[] = L"\xE77F";
constexpr wchar_t kGlyphRefresh[] = L"\xE72C";
constexpr wchar_t kGlyphApps[] = L"\xE71D";
constexpr wchar_t kGlyphChevron[] = L"\xE76C";
constexpr wchar_t kGlyphFolder[] = L"\xE8B7";
constexpr wchar_t kGlyphExit[] = L"\xE8BB";

// Clicking the tray icon while the panel is open first deactivates the panel
// (it hides), then delivers the click - which must not reopen it.
constexpr auto kReopenGuard = std::chrono::milliseconds(300);

enum class Hit : std::uint8_t { None, Toggle, Paste, Refresh, Apps, Folder, Exit };

D2D1_RECT_F HitRect(Hit hit) {
  const float right = kWidth - kPad;
  switch (hit) {
    case Hit::Toggle: return {kPad, kRowsTop, right, kRowsTop + kRow};
    case Hit::Refresh: {
      const float top = kRowsTop + kRow + (kRow - kButton) / 2;
      return {right - kButton, top, right, top + kButton};
    }
    case Hit::Paste: {
      const float top = kRowsTop + kRow + (kRow - kButton) / 2;
      return {right - 2 * kButton - 4, top, right - kButton - 4, top + kButton};
    }
    case Hit::Apps: return {kPad, kRowsTop + 2 * kRow, right, kRowsTop + 3 * kRow};
    case Hit::Folder: return {kPad, kFooterTop, kWidth / 2 - 2, kFooterTop + kFooter};
    case Hit::Exit: return {kWidth / 2 + 2, kFooterTop, right, kFooterTop + kFooter};
    case Hit::None: break;
  }
  return {};
}

bool Contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

D2D1_COLOR_F Rgb(float r, float g, float b, float a = 1.0f) { return D2D1::ColorF(r / 255, g / 255, b / 255, a); }

D2D1_COLOR_F FromColorRef(COLORREF c) { return Rgb(GetRValue(c), GetGValue(c), GetBValue(c)); }

// A glyph font that exists here: Fluent Icons on Windows 11, MDL2 on 10.
std::wstring GlyphFamily(IDWriteFactory* factory) {
  wil::com_ptr<IDWriteFontCollection> fonts;
  if (SUCCEEDED(factory->GetSystemFontCollection(&fonts, FALSE))) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &exists)) && exists) {
      return L"Segoe Fluent Icons";
    }
  }
  return L"Segoe MDL2 Assets";
}

}  // namespace

struct Flyout::Impl {
  HINSTANCE instance;
  CommandHandler onCommand;
  wil::unique_hwnd window;
  FlyoutContent content;
  Hit hover = Hit::None;
  bool visible = false;
  std::chrono::steady_clock::time_point hiddenAt{};

  wil::com_ptr<ID2D1Factory> d2d;
  wil::com_ptr<IDWriteFactory> dwrite;
  wil::com_ptr<ID2D1HwndRenderTarget> target;
  wil::com_ptr<IDWriteTextFormat> title;
  wil::com_ptr<IDWriteTextFormat> body;
  wil::com_ptr<IDWriteTextFormat> caption;
  wil::com_ptr<IDWriteTextFormat> glyph;
  wil::com_ptr<IDWriteInlineObject> ellipsis;

  Impl(HINSTANCE inst, CommandHandler handler) : instance(inst), onCommand(std::move(handler)) {
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.put()));
    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite.put())));
    const auto format = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size,
                            wil::com_ptr<IDWriteTextFormat>& out) {
      THROW_IF_FAILED(dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, size, L"ru-ru", out.put()));
      out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    format(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 17, title);
    format(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 14, body);
    format(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 12.5f, caption);
    format(GlyphFamily(dwrite.get()).c_str(), DWRITE_FONT_WEIGHT_NORMAL, 16, glyph);
    glyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    THROW_IF_FAILED(dwrite->CreateEllipsisTrimmingSign(body.get(), ellipsis.put()));
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    body->SetTrimming(&trimming, ellipsis.get());
    caption->SetTrimming(&trimming, ellipsis.get());

    WNDCLASSW wc{};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = &Impl::WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);  // already registered is fine
    window.reset(CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"Sovereign", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, instance, this));
    THROW_LAST_ERROR_IF(!window);

    // Windows 11 look: rounded corners, dark frame, a thin blue border.
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window.get(), DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof corners);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window.get(), DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    const COLORREF border = RGB(46, 74, 128);
    DwmSetWindowAttribute(window.get(), DWMWA_BORDER_COLOR, &border, sizeof border);
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
      // The creation parameter arrives as an LPARAM - the cast is the API's shape.
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);  // NOLINT(performance-no-int-to-ptr)
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));  // NOLINT(performance-no-int-to-ptr)
    return self != nullptr ? self->Handle(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
  }

  float Dpi() const { return static_cast<float>(GetDpiForWindow(window.get())); }

  Hit HitAt(LPARAM lParam) const {
    const float scale = 96.0f / Dpi();
    const float x = static_cast<float>(GET_X_LPARAM(lParam)) * scale;
    const float y = static_cast<float>(GET_Y_LPARAM(lParam)) * scale;
    for (const Hit h : {Hit::Paste, Hit::Refresh, Hit::Toggle, Hit::Apps, Hit::Folder, Hit::Exit}) {
      if (Contains(HitRect(h), x, y)) {
        return (h == Hit::Refresh && !content.hasSubscription) ? Hit::None : h;
      }
    }
    return Hit::None;
  }

  POINT ScreenPoint(Hit hit) const {
    const D2D1_RECT_F r = HitRect(hit);
    const float scale = Dpi() / 96.0f;
    POINT p{static_cast<LONG>(r.right * scale), static_cast<LONG>(r.top * scale)};
    ClientToScreen(window.get(), &p);
    return p;
  }

  LRESULT Handle(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
      case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_MOUSEMOVE: {
        const Hit h = HitAt(lParam);
        if (h != hover) {
          hover = h;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
      }
      case WM_MOUSELEAVE:
        hover = Hit::None;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_LBUTTONUP:
        Click(HitAt(lParam));
        return 0;
      case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
          HideNow();
        }
        return 0;
      case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
          HideNow();
        }
        return 0;
      case WM_SIZE:
        if (target) {
          target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        return 0;
      default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
  }

  void Click(Hit hit) {
    switch (hit) {
      case Hit::Toggle: onCommand(FlyoutCommand::Toggle, {}); break;
      case Hit::Paste: onCommand(FlyoutCommand::PasteSubscription, {}); break;
      case Hit::Refresh: onCommand(FlyoutCommand::RefreshSubscription, {}); break;
      case Hit::Apps: {
        const POINT anchor = ScreenPoint(Hit::Apps);
        HideNow();
        onCommand(FlyoutCommand::Apps, anchor);
        break;
      }
      case Hit::Folder: HideNow(); onCommand(FlyoutCommand::OpenFolder, {}); break;
      case Hit::Exit: HideNow(); onCommand(FlyoutCommand::Exit, {}); break;
      case Hit::None: break;
    }
  }

  void HideNow() {
    if (visible) {
      visible = false;
      hiddenAt = std::chrono::steady_clock::now();
      ShowWindow(window.get(), SW_HIDE);
    }
  }

  void ShowAt(const RECT& anchor) {
    // Size and place in pixels of the monitor the tray icon is on: above a
    // bottom taskbar, below a top one, beside a side one; inside the work area.
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(monitor, &mi);
    UINT dpiX = 96;
    UINT dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float scale = static_cast<float>(dpiX) / 96.0f;
    const auto w = static_cast<int>(std::lround(kWidth * scale));
    const auto h = static_cast<int>(std::lround(kHeight * scale));
    const int margin = static_cast<int>(12 * scale);
    const RECT work = mi.rcWork;
    const LONG cx = (anchor.left + anchor.right) / 2;
    const LONG cy = (anchor.top + anchor.bottom) / 2;
    LONG x = std::clamp<LONG>(cx - w / 2, work.left + margin, work.right - margin - w);
    LONG y = work.bottom - margin - h;
    if (anchor.bottom <= work.top) {
      y = work.top + margin;
    } else if (anchor.left >= work.right) {
      x = work.right - margin - w;
      y = std::clamp<LONG>(cy - h / 2, work.top + margin, work.bottom - margin - h);
    } else if (anchor.right <= work.left) {
      x = work.left + margin;
      y = std::clamp<LONG>(cy - h / 2, work.top + margin, work.bottom - margin - h);
    }
    SetWindowPos(window.get(), HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    target.reset();  // the DPI may differ from last time
    hover = Hit::None;
    visible = true;
    ShowWindow(window.get(), SW_SHOW);
    SetForegroundWindow(window.get());
  }

  void Paint() {
    if (!target) {
      RECT rc{};
      GetClientRect(window.get(), &rc);
      const D2D1_RENDER_TARGET_PROPERTIES props =
          D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), Dpi(), Dpi());
      if (FAILED(d2d->CreateHwndRenderTarget(props,
                                             D2D1::HwndRenderTargetProperties(window.get(), D2D1::SizeU(rc.right, rc.bottom)),
                                             target.put()))) {
        return;
      }
      target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    }
    auto* t = target.get();
    t->BeginDraw();
    t->Clear(Rgb(28, 31, 38));

    wil::com_ptr<ID2D1SolidColorBrush> primary;
    wil::com_ptr<ID2D1SolidColorBrush> secondary;
    wil::com_ptr<ID2D1SolidColorBrush> hoverFill;
    wil::com_ptr<ID2D1SolidColorBrush> line;
    wil::com_ptr<ID2D1SolidColorBrush> accent;
    wil::com_ptr<ID2D1SolidColorBrush> brush;
    t->CreateSolidColorBrush(Rgb(242, 244, 248), primary.put());
    t->CreateSolidColorBrush(Rgb(160, 168, 184), secondary.put());
    t->CreateSolidColorBrush(Rgb(255, 255, 255, 0.07f), hoverFill.put());
    t->CreateSolidColorBrush(Rgb(255, 255, 255, 0.08f), line.put());
    t->CreateSolidColorBrush(Rgb(76, 146, 255), accent.put());
    t->CreateSolidColorBrush(Rgb(0, 0, 0), brush.put());
    if (!primary || !secondary || !hoverFill || !line || !accent || !brush) {
      t->EndDraw();
      return;
    }

    const auto text = [&](const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, ID2D1Brush* b) {
      t->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const auto hoverBox = [&](Hit h) {
      if (hover == h) {
        t->FillRoundedRectangle(D2D1::RoundedRect(HitRect(h), 6, 6), hoverFill.get());
      }
    };
    const auto rowIcon = [&](const wchar_t* g, float top) {
      text(g, glyph.get(), {kPad + 6, top, kPad + 34, top + kRow}, primary.get());
    };
    const float right = kWidth - kPad;

    // Header: the mark (a stand-in until the real icon), name, live status.
    {
      const D2D1_RECT_F mark{kPad + 2, kPad + 11, kPad + 38, kPad + 47};
      brush->SetColor(Rgb(36, 44, 62));
      t->FillRoundedRectangle(D2D1::RoundedRect(mark, 8, 8), brush.get());
      brush->SetColor(Rgb(64, 140, 255));
      const float cell = 7;
      const float gap = 2;
      const float x0 = (mark.left + mark.right) / 2 - cell - gap / 2;
      const float y0 = (mark.top + mark.bottom) / 2 - cell - gap / 2;
      for (int i = 0; i < 4; ++i) {
        const float x = x0 + (i % 2 == 0 ? 0.0f : cell + gap);
        const float y = y0 + (i < 2 ? 0.0f : cell + gap);
        t->FillRoundedRectangle(D2D1::RoundedRect({x, y, x + cell, y + cell}, 1.5f, 1.5f), brush.get());
      }
      const float tx = kPad + 50;
      text(L"Sovereign", title.get(), {tx, kPad + 4, right, kPad + 30}, primary.get());
      brush->SetColor(FromColorRef(content.statusDot));
      t->FillEllipse(D2D1::Ellipse({tx + 4, kPad + 42}, 4, 4), brush.get());
      text(content.status, caption.get(), {tx + 14, kPad + 32, right, kPad + 52}, secondary.get());
      t->DrawLine({kPad, kRowsTop - kGap / 2}, {right, kRowsTop - kGap / 2}, line.get());
    }

    // Row 1: on/off with a switch.
    {
      const float top = kRowsTop;
      hoverBox(Hit::Toggle);
      rowIcon(kGlyphPower, top);
      text(content.on ? L"Включено" : L"Выключено", body.get(), {kPad + 42, top, right - 56, top + kRow}, primary.get());
      const D2D1_RECT_F track{right - 50, top + 10, right - 10, top + 30};
      const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(track, 10, 10);
      if (content.on) {
        t->FillRoundedRectangle(pill, accent.get());
        brush->SetColor(Rgb(255, 255, 255));
        t->FillEllipse(D2D1::Ellipse({track.right - 10, top + 20}, 6, 6), brush.get());
      } else {
        t->DrawRoundedRectangle(pill, secondary.get(), 1.2f);
        t->FillEllipse(D2D1::Ellipse({track.left + 10, top + 20}, 5, 5), secondary.get());
      }
    }

    // Row 2: subscription state, paste and refresh buttons.
    {
      const float top = kRowsTop + kRow;
      rowIcon(kGlyphSync, top);
      text(L"Подписка · " + content.subscription, body.get(),
           {kPad + 42, top, HitRect(Hit::Paste).left - 6, top + kRow}, primary.get());
      for (const auto& [h, g] : {std::pair{Hit::Paste, kGlyphPaste}, std::pair{Hit::Refresh, kGlyphRefresh}}) {
        hoverBox(h);
        const bool enabled = h != Hit::Refresh || content.hasSubscription;
        text(g, glyph.get(), HitRect(h), enabled ? primary.get() : line.get());
      }
    }

    // Row 3: per-app routing.
    {
      const float top = kRowsTop + 2 * kRow;
      hoverBox(Hit::Apps);
      rowIcon(kGlyphApps, top);
      text(content.apps, body.get(), {kPad + 42, top, right - 30, top + kRow}, primary.get());
      text(kGlyphChevron, glyph.get(), {right - 30, top, right - 6, top + kRow}, secondary.get());
    }

    // Footer: settings folder and exit side by side.
    t->DrawLine({kPad, kFooterTop - kGap / 2}, {right, kFooterTop - kGap / 2}, line.get());
    using FooterItem = std::tuple<Hit, const wchar_t*, const wchar_t*>;
    for (const auto& [h, g, label] : {FooterItem{Hit::Folder, kGlyphFolder, L"Папка настроек"},
                                      FooterItem{Hit::Exit, kGlyphExit, L"Выход"}}) {
      hoverBox(h);
      const D2D1_RECT_F r = HitRect(h);
      text(g, glyph.get(), {r.left + 4, r.top, r.left + 32, r.bottom}, secondary.get());
      text(label, caption.get(), {r.left + 36, r.top, r.right - 4, r.bottom}, secondary.get());
    }

    if (t->EndDraw() == D2DERR_RECREATE_TARGET) {
      target.reset();
    }
  }
};

Flyout::Flyout(HINSTANCE instance, CommandHandler onCommand)
    : impl_(std::make_unique<Impl>(instance, std::move(onCommand))) {}

Flyout::~Flyout() = default;

void Flyout::Toggle(const RECT& anchor, const FlyoutContent& content) {
  impl_->content = content;
  if (impl_->visible) {
    impl_->HideNow();
    return;
  }
  if (std::chrono::steady_clock::now() - impl_->hiddenAt < kReopenGuard) {
    return;  // this click is what just closed it
  }
  impl_->ShowAt(anchor);
}

void Flyout::Hide() { impl_->HideNow(); }

void Flyout::Update(const FlyoutContent& content) {
  impl_->content = content;
  if (impl_->visible) {
    InvalidateRect(impl_->window.get(), nullptr, FALSE);
  }
}

}  // namespace sovereign::tray
