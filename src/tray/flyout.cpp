#include "flyout.h"

#include <windowsx.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <wincodec.h>

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "icons.h"

namespace sovereign::tray {

namespace {

constexpr wchar_t kClassName[] = L"SovereignTrayFlyout";

// Layout, in DIPs (1/96 inch): the render target works in DIPs at the
// monitor's DPI, so nothing below needs scaling by hand.
constexpr float kWidth = 340;
constexpr float kPad = 12;
constexpr float kHeader = 58;
constexpr float kRow = 40;
constexpr float kListRow = 34;
constexpr float kFooter = 38;
constexpr float kGap = 6;
constexpr float kButton = 28;
constexpr std::size_t kVisibleApps = 7;  // more scroll with the wheel

// Segoe Fluent Icons (Windows 11; the same code points in Segoe MDL2 Assets).
constexpr const wchar_t* kGlyphPower = L"\xE7E8";
constexpr const wchar_t* kGlyphGlobe = L"\xE774";
constexpr const wchar_t* kGlyphSync = L"\xE895";
constexpr const wchar_t* kGlyphPaste = L"\xE77F";
constexpr const wchar_t* kGlyphRefresh = L"\xE72C";
constexpr const wchar_t* kGlyphApps = L"\xE71D";
constexpr const wchar_t* kGlyphChevron = L"\xE76C";
constexpr const wchar_t* kGlyphBack = L"\xE72B";
constexpr const wchar_t* kGlyphFolder = L"\xE8B7";
constexpr const wchar_t* kGlyphExit = L"\xE8BB";
constexpr const wchar_t* kGlyphRemove = L"\xE711";
constexpr const wchar_t* kGlyphAdd = L"\xE710";
constexpr const wchar_t* kGlyphFile = L"\xE8E5";
constexpr const wchar_t* kGlyphCheck = L"\xE73E";

// Clicking the tray icon while the panel is open first deactivates the panel
// (it hides), then delivers the click - which must not reopen it.
constexpr auto kReopenGuard = std::chrono::milliseconds(300);

enum class Page : std::uint8_t { Main, Apps, Protocols };

enum class Kind : std::uint8_t {
  Toggle,      // row with a switch
  Nav,         // row with a chevron: opens a page
  Label,       // glyph + text, not clickable
  IconButton,  // a small square button with a glyph
  Back,        // a page's title row, goes back to the main page
  Segment,     // half of the two-way mode switch
  Choice,      // a pickable row, checked when current
  Footer,      // glyph + caption, bottom row
};

enum class Target : std::uint8_t { None, Command, OpenApps, OpenProtocols, BackToMain };

struct Item {
  Kind kind = Kind::Label;
  D2D1_RECT_F rect{};
  const wchar_t* glyph = nullptr;
  std::wstring text;
  Target target = Target::None;
  FlyoutCommand command = FlyoutCommand::Toggle;
  int index = 0;
  bool checked = false;
  bool enabled = true;
};

struct Layout {
  std::vector<Item> items;
  std::vector<float> separators;  // y of horizontal lines
  float height = 0;
  bool header = false;
};

D2D1_RECT_F Row(float top, float height) { return {kPad, top, kWidth - kPad, top + height}; }

bool Contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

D2D1_COLOR_F Rgb(float r, float g, float b, float a = 1.0f) { return D2D1::ColorF(r / 255, g / 255, b / 255, a); }

D2D1_COLOR_F FromColorRef(COLORREF c) {
  return Rgb(static_cast<float>(GetRValue(c)), static_cast<float>(GetGValue(c)), static_cast<float>(GetBValue(c)));
}

Item Command(Kind kind, D2D1_RECT_F rect, const wchar_t* glyph, std::wstring text, FlyoutCommand command, int index = 0) {
  Item i;
  i.kind = kind;
  i.rect = rect;
  i.glyph = glyph;
  i.text = std::move(text);
  i.target = Target::Command;
  i.command = command;
  i.index = index;
  return i;
}

Item Link(Kind kind, D2D1_RECT_F rect, const wchar_t* glyph, std::wstring text, Target target) {
  Item i;
  i.kind = kind;
  i.rect = rect;
  i.glyph = glyph;
  i.text = std::move(text);
  i.target = target;
  return i;
}

Item Text(D2D1_RECT_F rect, const wchar_t* glyph, std::wstring text, bool enabled = true) {
  Item i;
  i.kind = Kind::Label;
  i.rect = rect;
  i.glyph = glyph;
  i.text = std::move(text);
  i.enabled = enabled;
  return i;
}

Layout MainPage(const FlyoutContent& c) {
  Layout l;
  l.header = true;
  float y = kPad + kHeader + kGap;
  l.separators.push_back(y - kGap / 2);

  Item toggle = Command(Kind::Toggle, Row(y, kRow), kGlyphPower, c.on ? L"Включено" : L"Выключено", FlyoutCommand::Toggle);
  toggle.checked = c.on;
  l.items.push_back(std::move(toggle));
  y += kRow;

  if (c.protocols.empty()) {
    l.items.push_back(Text(Row(y, kRow), kGlyphGlobe, L"Протокол · в конфиге нет выбора", false));
  } else {
    const std::wstring current =
        c.protocol >= 0 && c.protocol < static_cast<int>(c.protocols.size()) ? c.protocols[c.protocol] : L"?";
    l.items.push_back(Link(Kind::Nav, Row(y, kRow), kGlyphGlobe, L"Протокол · " + current, Target::OpenProtocols));
  }
  y += kRow;

  const float right = kWidth - kPad;
  const float buttonTop = y + (kRow - kButton) / 2;
  const D2D1_RECT_F refresh{right - kButton, buttonTop, right, buttonTop + kButton};
  const D2D1_RECT_F paste{refresh.left - 4 - kButton, buttonTop, refresh.left - 4, buttonTop + kButton};
  l.items.push_back(Text({kPad, y, paste.left - 4, y + kRow}, kGlyphSync, L"Подписка · " + c.subscription));
  l.items.push_back(Command(Kind::IconButton, paste, kGlyphPaste, L"", FlyoutCommand::PasteSubscription));
  Item refreshItem = Command(Kind::IconButton, refresh, kGlyphRefresh, L"", FlyoutCommand::RefreshSubscription);
  refreshItem.enabled = c.hasSubscription;
  l.items.push_back(std::move(refreshItem));
  y += kRow;

  const std::wstring apps = c.apps.empty()
                                ? std::wstring(L"Приложения · всё через VPN")
                                : L"Приложения (" + std::to_wstring(c.apps.size()) + L") · " +
                                      (c.appsInclude ? L"только список" : L"кроме списка");
  l.items.push_back(Link(Kind::Nav, Row(y, kRow), kGlyphApps, apps, Target::OpenApps));
  y += kRow + kGap;

  l.separators.push_back(y - kGap / 2);
  l.items.push_back(Command(Kind::Footer, {kPad, y, kWidth / 2 - 2, y + kFooter}, kGlyphFolder, L"Папка настроек",
                            FlyoutCommand::OpenFolder));
  l.items.push_back(Command(Kind::Footer, {kWidth / 2 + 2, y, right, y + kFooter}, kGlyphExit, L"Выход",
                            FlyoutCommand::Exit));
  l.height = y + kFooter + kPad;
  return l;
}

Layout AppsPage(const FlyoutContent& c, std::size_t scroll) {
  Layout l;
  float y = kPad;
  l.items.push_back(Link(Kind::Back, Row(y, 36), kGlyphBack, L"Приложения", Target::BackToMain));
  y += 36 + kGap;

  const float mid = kWidth / 2;
  Item except = Command(Kind::Segment, {kPad, y, mid - 2, y + 32}, nullptr, L"Все, кроме списка", FlyoutCommand::SetAppsMode, 0);
  except.checked = !c.appsInclude;
  Item only = Command(Kind::Segment, {mid + 2, y, kWidth - kPad, y + 32}, nullptr, L"Только список", FlyoutCommand::SetAppsMode, 1);
  only.checked = c.appsInclude;
  l.items.push_back(std::move(except));
  l.items.push_back(std::move(only));
  y += 32 + kGap + 2;

  if (c.apps.empty()) {
    l.items.push_back(Text(Row(y, kListRow), nullptr, L"список пуст — всё через VPN", false));
    y += kListRow;
  }
  const std::size_t shown = std::min(kVisibleApps, c.apps.size());
  for (std::size_t n = 0; n < shown; ++n) {
    const std::size_t i = std::min(scroll, c.apps.size() - shown) + n;
    const float right = kWidth - kPad;
    const float top = y + (kListRow - kButton) / 2;
    l.items.push_back(Text({kPad, y, right - kButton - 4, y + kListRow}, nullptr, c.apps[i]));
    l.items.push_back(Command(Kind::IconButton, {right - kButton, top, right, top + kButton}, kGlyphRemove, L"",
                              FlyoutCommand::RemoveApp, static_cast<int>(i)));
    y += kListRow;
  }
  if (c.apps.size() > kVisibleApps) {
    l.items.push_back(Text(Row(y, 22), nullptr,
                           L"прокрутка колесом: ещё " + std::to_wstring(c.apps.size() - kVisibleApps), false));
    y += 22;
  }
  y += kGap;

  l.separators.push_back(y - kGap / 2);
  l.items.push_back(Command(Kind::Footer, {kPad, y, kWidth / 2 - 2, y + kFooter}, kGlyphAdd, L"Из запущенных",
                            FlyoutCommand::AddRunning));
  l.items.push_back(Command(Kind::Footer, {kWidth / 2 + 2, y, kWidth - kPad, y + kFooter}, kGlyphFile,
                            L"Выбрать exe…", FlyoutCommand::AddExe));
  l.height = y + kFooter + kPad;
  return l;
}

Layout ProtocolsPage(const FlyoutContent& c) {
  Layout l;
  float y = kPad;
  l.items.push_back(Link(Kind::Back, Row(y, 36), kGlyphBack, L"Протокол", Target::BackToMain));
  y += 36 + kGap;
  for (std::size_t i = 0; i < c.protocols.size(); ++i) {
    const std::wstring& name = c.protocols[i];
    Item choice = Command(Kind::Choice, Row(y, kListRow), kGlyphCheck,
                          name == L"auto" ? L"auto — лучший по задержке" : name, FlyoutCommand::SetProtocol,
                          static_cast<int>(i));
    choice.checked = static_cast<int>(i) == c.protocol;
    l.items.push_back(std::move(choice));
    y += kListRow;
  }
  l.height = y + kPad;
  return l;
}

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
  Page page = Page::Main;
  std::size_t scroll = 0;
  Layout layout;
  int hover = -1;
  bool visible = false;
  RECT anchor{};
  std::chrono::steady_clock::time_point hiddenAt{};

  wil::com_ptr<ID2D1Factory> d2d;
  wil::com_ptr<IDWriteFactory> dwrite;
  wil::com_ptr<IWICImagingFactory> wic;
  wil::com_ptr<ID2D1HwndRenderTarget> target;
  wil::com_ptr<ID2D1Bitmap> mark;  // the app icon for the header; tied to the target
  wil::com_ptr<IDWriteTextFormat> title;
  wil::com_ptr<IDWriteTextFormat> body;
  wil::com_ptr<IDWriteTextFormat> caption;
  wil::com_ptr<IDWriteTextFormat> centered;
  wil::com_ptr<IDWriteTextFormat> glyph;
  wil::com_ptr<IDWriteInlineObject> ellipsis;

  Impl(HINSTANCE inst, CommandHandler handler) : instance(inst), onCommand(std::move(handler)) {
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.put()));
    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite.put())));
    wic = wil::CoCreateInstanceNoThrow<IWICImagingFactory>(CLSID_WICImagingFactory);  // header icon only
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
    format(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 13, centered);
    centered->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
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
    // IDC_ARROW (32512) spelled out: the macro goes through the UNICODE-dependent
    // MAKEINTRESOURCE, which the CI's clang won't match with LoadCursorW.
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
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

  Layout Build() const {
    switch (page) {
      case Page::Apps: return AppsPage(content, scroll);
      case Page::Protocols: return ProtocolsPage(content);
      case Page::Main: break;
    }
    return MainPage(content);
  }

  int HitAt(LPARAM lParam) const {
    const float scale = 96.0f / Dpi();
    const float x = static_cast<float>(GET_X_LPARAM(lParam)) * scale;
    const float y = static_cast<float>(GET_Y_LPARAM(lParam)) * scale;
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
      const Item& item = layout.items[i];
      if (item.target != Target::None && item.enabled && Contains(item.rect, x, y)) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  POINT ScreenPoint(const D2D1_RECT_F& r) const {
    const float scale = Dpi() / 96.0f;
    POINT p{static_cast<LONG>(r.left * scale), static_cast<LONG>(r.bottom * scale)};
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
        const int h = HitAt(lParam);
        if (h != hover) {
          hover = h;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
      }
      case WM_MOUSELEAVE:
        hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_MOUSEWHEEL:
        if (page == Page::Apps && content.apps.size() > kVisibleApps) {
          const std::size_t max = content.apps.size() - kVisibleApps;
          if (GET_WHEEL_DELTA_WPARAM(wParam) < 0) {
            scroll = std::min(max, scroll + 1);
          } else if (scroll > 0) {
            --scroll;
          }
          Relayout();
        }
        return 0;
      case WM_LBUTTONUP: {
        const int h = HitAt(lParam);
        if (h >= 0) {
          Click(layout.items[static_cast<std::size_t>(h)]);
        }
        return 0;
      }
      case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
          if (page != Page::Main) {
            Show(Page::Main);
          } else {
            HideNow();
          }
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

  void Click(const Item& item) {
    switch (item.target) {
      case Target::OpenApps: Show(Page::Apps); return;
      case Target::OpenProtocols: Show(Page::Protocols); return;
      case Target::BackToMain: Show(Page::Main); return;
      case Target::None: return;
      case Target::Command: break;
    }
    FlyoutArgs args;
    args.index = item.index;
    args.anchor = ScreenPoint(item.rect);
    switch (item.command) {
      // These take the focus (a menu, a file dialog) or end the tray: close first.
      case FlyoutCommand::AddRunning:
      case FlyoutCommand::AddExe:
      case FlyoutCommand::OpenFolder:
      case FlyoutCommand::Exit:
        HideNow();
        break;
      case FlyoutCommand::SetProtocol:
        page = Page::Main;  // picked: back to the overview, which shows it
        break;
      default:
        break;
    }
    onCommand(item.command, args);
    if (visible) {
      Relayout();
    }
  }

  void HideNow() {
    if (visible) {
      visible = false;
      hiddenAt = std::chrono::steady_clock::now();
      ShowWindow(window.get(), SW_HIDE);
    }
  }

  void Show(Page p) {
    page = p;
    scroll = 0;
    hover = -1;
    Relayout();
  }

  // Lays the current page out and fits the window to it, anchored to the tray
  // icon: above a bottom taskbar, below a top one, beside a side one; inside
  // the work area of that monitor, in its DPI.
  void Relayout() {
    layout = Build();
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(monitor, &mi);
    UINT dpiX = 96;
    UINT dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float scale = static_cast<float>(dpiX) / 96.0f;
    const auto w = static_cast<LONG>(std::lround(kWidth * scale));
    const auto h = static_cast<LONG>(std::lround(layout.height * scale));
    const auto margin = static_cast<LONG>(std::lround(12 * scale));
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
    InvalidateRect(window.get(), nullptr, FALSE);
  }

  void Open(const RECT& at) {
    anchor = at;
    target.reset();  // the DPI may differ from last time
    mark.reset();
    visible = true;
    Show(Page::Main);
    ShowWindow(window.get(), SW_SHOW);
    SetForegroundWindow(window.get());
  }

  // The app icon as a Direct2D bitmap at the header's size in pixels.
  void EnsureMark(float dip) {
    if (mark || !wic || !target) {
      return;
    }
    const auto px = static_cast<int>(std::lround(dip * Dpi() / 96.0f));
    const wil::unique_hicon icon = LoadAppIcon(px);
    wil::com_ptr<IWICBitmap> bitmap;
    wil::com_ptr<IWICFormatConverter> converter;
    if (!icon || FAILED(wic->CreateBitmapFromHICON(icon.get(), bitmap.put())) ||
        FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(bitmap.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) {
      return;
    }
    target->CreateBitmapFromWicBitmap(converter.get(), nullptr, mark.put());
  }

  void Paint() {
    if (!target) {
      RECT rc{};
      GetClientRect(window.get(), &rc);
      const D2D1_RENDER_TARGET_PROPERTIES props =
          D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), Dpi(), Dpi());
      if (FAILED(d2d->CreateHwndRenderTarget(
              props, D2D1::HwndRenderTargetProperties(window.get(), D2D1::SizeU(rc.right, rc.bottom)), target.put()))) {
        return;
      }
      target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
      mark.reset();
    }
    auto* t = target.get();
    t->BeginDraw();
    t->Clear(Rgb(28, 31, 38));

    wil::com_ptr<ID2D1SolidColorBrush> primary;
    wil::com_ptr<ID2D1SolidColorBrush> secondary;
    wil::com_ptr<ID2D1SolidColorBrush> faint;
    wil::com_ptr<ID2D1SolidColorBrush> hoverFill;
    wil::com_ptr<ID2D1SolidColorBrush> accent;
    wil::com_ptr<ID2D1SolidColorBrush> brush;
    t->CreateSolidColorBrush(Rgb(242, 244, 248), primary.put());
    t->CreateSolidColorBrush(Rgb(160, 168, 184), secondary.put());
    t->CreateSolidColorBrush(Rgb(255, 255, 255, 0.10f), faint.put());
    t->CreateSolidColorBrush(Rgb(255, 255, 255, 0.07f), hoverFill.put());
    t->CreateSolidColorBrush(Rgb(76, 146, 255), accent.put());
    t->CreateSolidColorBrush(Rgb(0, 0, 0), brush.put());
    if (!primary || !secondary || !faint || !hoverFill || !accent || !brush) {
      t->EndDraw();
      return;
    }

    const auto text = [&](const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, ID2D1Brush* b) {
      t->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const float right = kWidth - kPad;

    if (layout.header) {
      const D2D1_RECT_F markRect{kPad, kPad + 10, kPad + 40, kPad + 50};
      EnsureMark(markRect.right - markRect.left);
      if (mark) {
        t->DrawBitmap(mark.get(), markRect);
      }
      const float tx = kPad + 50;
      text(L"Sovereign", title.get(), {tx, kPad + 4, right, kPad + 30}, primary.get());
      brush->SetColor(FromColorRef(content.statusDot));
      t->FillEllipse(D2D1::Ellipse({tx + 4, kPad + 42}, 4, 4), brush.get());
      text(content.status, caption.get(), {tx + 14, kPad + 32, right, kPad + 52}, secondary.get());
    }
    for (const float y : layout.separators) {
      t->DrawLine({kPad, y}, {right, y}, faint.get());
    }

    for (std::size_t i = 0; i < layout.items.size(); ++i) {
      const Item& it = layout.items[i];
      const D2D1_RECT_F r = it.rect;
      const bool hovered = static_cast<int>(i) == hover;
      if (hovered && it.kind != Kind::Segment) {
        t->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), hoverFill.get());
      }
      ID2D1Brush* ink = it.enabled ? primary.get() : secondary.get();
      const D2D1_RECT_F glyphBox{r.left + 6, r.top, r.left + 34, r.bottom};
      const D2D1_RECT_F textBox{r.left + (it.glyph != nullptr ? 42.0f : 8.0f), r.top, r.right - 8, r.bottom};
      switch (it.kind) {
        case Kind::Toggle: {
          text(it.glyph, glyph.get(), glyphBox, primary.get());
          text(it.text, body.get(), {textBox.left, r.top, r.right - 56, r.bottom}, primary.get());
          const float mid = (r.top + r.bottom) / 2;
          const D2D1_RECT_F track{r.right - 50, mid - 10, r.right - 10, mid + 10};
          const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(track, 10, 10);
          if (it.checked) {
            t->FillRoundedRectangle(pill, accent.get());
            brush->SetColor(Rgb(255, 255, 255));
            t->FillEllipse(D2D1::Ellipse({track.right - 10, mid}, 6, 6), brush.get());
          } else {
            t->DrawRoundedRectangle(pill, secondary.get(), 1.2f);
            t->FillEllipse(D2D1::Ellipse({track.left + 10, mid}, 5, 5), secondary.get());
          }
          break;
        }
        case Kind::Nav:
          text(it.glyph, glyph.get(), glyphBox, primary.get());
          text(it.text, body.get(), {textBox.left, r.top, r.right - 30, r.bottom}, primary.get());
          text(kGlyphChevron, glyph.get(), {r.right - 30, r.top, r.right - 6, r.bottom}, secondary.get());
          break;
        case Kind::Label:
          if (it.glyph != nullptr) {
            text(it.glyph, glyph.get(), glyphBox, ink);
          }
          text(it.text, it.enabled ? body.get() : caption.get(), textBox, ink);
          break;
        case Kind::IconButton:
          text(it.glyph, glyph.get(), r, it.enabled ? primary.get() : faint.get());
          break;
        case Kind::Back:
          text(it.glyph, glyph.get(), glyphBox, primary.get());
          text(it.text, title.get(), textBox, primary.get());
          break;
        case Kind::Segment: {
          const D2D1_ROUNDED_RECT box = D2D1::RoundedRect(r, 6, 6);
          if (it.checked) {
            t->FillRoundedRectangle(box, accent.get());
          } else {
            t->DrawRoundedRectangle(box, hovered ? secondary.get() : faint.get(), 1.0f);
          }
          text(it.text, centered.get(), r, it.checked ? primary.get() : secondary.get());
          break;
        }
        case Kind::Choice:
          if (it.checked) {
            text(it.glyph, glyph.get(), glyphBox, accent.get());
          }
          text(it.text, body.get(), {r.left + 42, r.top, r.right - 8, r.bottom}, primary.get());
          break;
        case Kind::Footer:
          text(it.glyph, glyph.get(), {r.left + 4, r.top, r.left + 32, r.bottom}, secondary.get());
          text(it.text, caption.get(), {r.left + 36, r.top, r.right - 4, r.bottom}, secondary.get());
          break;
      }
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
  impl_->Open(anchor);
}

void Flyout::Hide() { impl_->HideNow(); }

void Flyout::Update(const FlyoutContent& content) {
  impl_->content = content;
  if (impl_->visible) {
    impl_->Relayout();
  }
}

}  // namespace sovereign::tray
