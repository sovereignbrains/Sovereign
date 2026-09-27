#include "main_window.h"

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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "icons.h"
#include "ui_style.h"

namespace sovereign::tray {

namespace {

using ui::FromColorRef;
using ui::Rgb;

constexpr wchar_t kClassName[] = L"SovereignMainWindow";

// Layout, in DIPs (1/96 inch): render targets work in DIPs at the window's
// DPI, so nothing below is scaled by hand.
constexpr float kDefaultWidth = 980;
constexpr float kDefaultHeight = 660;
constexpr float kMinWidth = 760;
constexpr float kMinHeight = 520;
constexpr float kRail = 232;           // the navigation rail
constexpr float kPadX = 36;            // the page's side margins
constexpr float kTop = 26;             // the page title's top
constexpr float kMaxPage = 820;        // wider windows keep the page at this
constexpr float kNavRow = 40;
constexpr float kRow = 48;             // a list or settings row
constexpr float kButton = 34;
constexpr float kGap = 16;             // between cards
constexpr float kRadius = 8;
constexpr float kWheelStep = 64;
constexpr std::size_t kHistory = 120;  // graph points: two minutes at a poll a second

// The log page: a line's height, and how many lines it keeps (the tray keeps
// no more either).
constexpr float kLogLine = 20;
constexpr std::size_t kLogKeep = 5000;

// How long the log's copy button says it did.
constexpr UINT_PTR kCopiedTimer = 1;
constexpr UINT kCopiedMs = 1500;

// Segoe Fluent Icons (Windows 11; the same code points in Segoe MDL2 Assets).
constexpr const wchar_t* kGlyphHome = L"\xE80F";
constexpr const wchar_t* kGlyphGlobe = L"\xE774";
constexpr const wchar_t* kGlyphSync = L"\xE895";
constexpr const wchar_t* kGlyphApps = L"\xE71D";
constexpr const wchar_t* kGlyphLog = L"\xE8A5";
constexpr const wchar_t* kGlyphSettings = L"\xE713";
constexpr const wchar_t* kGlyphPower = L"\xE7E8";
constexpr const wchar_t* kGlyphPaste = L"\xE77F";
constexpr const wchar_t* kGlyphRefresh = L"\xE72C";
constexpr const wchar_t* kGlyphRemove = L"\xE711";
constexpr const wchar_t* kGlyphAdd = L"\xE710";
constexpr const wchar_t* kGlyphFile = L"\xE8E5";
constexpr const wchar_t* kGlyphFolder = L"\xE8B7";
constexpr const wchar_t* kGlyphCopy = L"\xE8C8";
constexpr const wchar_t* kGlyphExit = L"\xE8BB";
constexpr const wchar_t* kGlyphCheck = L"\xE73E";
constexpr const wchar_t* kGlyphWarning = L"\xE7BA";
constexpr const wchar_t* kGlyphProgram = L"\xE7C4";
constexpr const wchar_t* kGlyphStopwatch = L"\xE916";

struct PageInfo {
  const wchar_t* glyph;
  const wchar_t* name;
};
constexpr PageInfo kPages[kUiPageCount] = {
    {kGlyphHome, L"Обзор"},       {kGlyphGlobe, L"Протокол"}, {kGlyphSync, L"Подписка"},
    {kGlyphApps, L"Приложения"},  {kGlyphLog, L"Журнал"},     {kGlyphSettings, L"Настройки"},
};

enum class Kind : std::uint8_t {
  Nav,           // a navigation rail entry
  Card,          // a rounded panel behind other items
  Divider,       // a line across a card
  Title,         // the page's title
  Heading,       // a card's title
  Text,          // one line
  Muted,         // one line, secondary
  Wrap,          // wrapped paragraph, secondary
  ErrorText,     // wrapped paragraph with a warning glyph, red
  Field,         // a label on the left, a value on the right
  Power,         // the overview's big on/off button
  StateText,     // the overview's state, large, with a wrapped detail
  Stat,          // a small card: caption and a large value
  Graph,         // the speed graph
  Button,        // a normal button
  AccentButton,  // the page's main action
  DangerButton,  // quit
  IconButton,    // a square button with a glyph
  Switch,        // a row with a toggle switch
  Choice,        // a pickable row with a radio mark
  Segment,       // half of a two-way switch
  AppRow,        // a program in the per-app list
  LogBox,        // the log's lines
  RailStatus,    // the rail's bottom: state dot and speed
};

enum class ItemAction : std::uint8_t { None, Command, Page, CopyLogs };

struct Item {
  Kind kind = Kind::Text;
  D2D1_RECT_F rect{};
  std::wstring_view glyph;  // empty: none (never a null pointer - /analyze)
  std::wstring text;
  std::wstring detail;
  ItemAction action = ItemAction::None;
  UiCommand command = UiCommand::Toggle;
  int index = 0;
  bool checked = false;
  bool enabled = true;
  bool scrolls = true;  // lives in the page (scrolls, clipped), not in the rail
};

struct Layout {
  std::vector<Item> items;
  float contentHeight = 0;  // the page's full height, for scrolling
  float scroll = 0;         // how far the page is scrolled
  float viewTop = 0;        // the scrolling viewport, in window DIPs
  float viewBottom = 0;
  std::optional<D2D1_RECT_F> logBox;
};

// The log page's state: its lines (newest last), how far they are scrolled
// and which are selected - from anchor to caret, inclusive.
struct LogView {
  std::deque<std::wstring> lines;
  float scroll = 0;     // DIPs from the first line
  bool follow = true;   // stays at the bottom as lines come
  int anchor = -1;      // -1: no selection
  int caret = -1;

  bool HasSelection() const { return anchor >= 0; }
  bool Selected(int i) const { return anchor >= 0 && i >= std::min(anchor, caret) && i <= std::max(anchor, caret); }
  float Height() const { return static_cast<float>(lines.size()) * kLogLine; }
};

// What a paint depends on besides the layout.
struct Interaction {
  int hover = -1;
  int pressed = -1;
  int focus = -1;
  bool focusVisible = false;
};

bool Contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

D2D1_RECT_F Inflate(const D2D1_RECT_F& r, float by) { return {r.left - by, r.top - by, r.right + by, r.bottom + by}; }

std::wstring_view Glyph(const wchar_t* glyph) { return glyph != nullptr ? std::wstring_view(glyph) : std::wstring_view(); }

Item Make(Kind kind, D2D1_RECT_F rect, std::wstring text = {}, const wchar_t* glyph = nullptr) {
  Item i;
  i.kind = kind;
  i.rect = rect;
  i.text = std::move(text);
  i.glyph = Glyph(glyph);
  return i;
}

Item CommandItem(Kind kind, D2D1_RECT_F rect, std::wstring text, const wchar_t* glyph, UiCommand command, int index = 0) {
  Item i = Make(kind, rect, std::move(text), glyph);
  i.action = ItemAction::Command;
  i.command = command;
  i.index = index;
  return i;
}

bool Interactive(const Item& item) { return item.action != ItemAction::None && item.enabled; }

// The overview's words for the state.
std::wstring StateTitle(const UiContent& c) {
  switch (c.display) {
    case Display::ServiceDown: return L"Служба не запущена";
    case Display::Off: return L"Выключено";
    case Display::Starting: return L"Подключение…";
    case Display::On: return L"Подключено";
    case Display::Error: return L"Ошибка";
  }
  return {};
}

std::wstring CurrentProtocol(const UiContent& c) {
  if (c.protocol >= 0 && c.protocol < static_cast<int>(c.protocols.size())) {
    return c.protocols[static_cast<std::size_t>(c.protocol)];
  }
  return {};
}

std::wstring StateDetail(const UiContent& c) {
  switch (c.display) {
    case Display::ServiceDown:
      return L"sovereign-core не отвечает. Установи службу от администратора: sovereign-core.exe --install";
    case Display::Off:
      return c.hasSubscription || !c.protocols.empty() ? L"Нажми на кнопку, чтобы подключиться."
                                                       : L"Сначала добавь подписку: скопируй ссылку и нажми «Вставить».";
    case Display::Starting: return L"Запускается ядро sing-box…";
    case Display::On: {
      std::wstring protocol = CurrentProtocol(c);
      if (!protocol.empty() && static_cast<std::size_t>(c.protocol) < c.delays.size() &&
          c.delays[static_cast<std::size_t>(c.protocol)].state == UiDelay::State::Ok) {
        protocol += std::format(L" ({} мс)", c.delays[static_cast<std::size_t>(c.protocol)].ms);
      }
      return (protocol.empty() ? std::wstring() : L"Протокол: " + protocol + L" · ") +
             std::format(L"соединений: {}", c.connections);
    }
    case Display::Error: return c.error.empty() ? std::wstring(L"ядро не запустилось") : c.error;
  }
  return {};
}

// Device-independent resources and everything between content and pixels:
// the layout of a page and its drawing onto any Direct2D target - a window's
// or, for snapshots, a WIC bitmap's.
class Painter {
 public:
  Painter() {
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()));
    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.put())));
    wic_ = wil::CoCreateInstanceNoThrow<IWICImagingFactory>(CLSID_WICImagingFactory);
    THROW_IF_FAILED(dwrite_->CreateEllipsisTrimmingSign(MakeFormat(kText, 14).get(), ellipsis_.put()));
    title_ = MakeFormat(kDisplay, 26, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    state_ = MakeFormat(kDisplay, 24, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    value_ = MakeFormat(kDisplay, 21, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    delay_ = MakeFormat(kText, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    delay_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    brand_ = MakeFormat(kDisplay, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    heading_ = MakeFormat(kText, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    body_ = MakeFormat(kText, 14);
    caption_ = MakeFormat(kText, 12.5f);
    button_ = MakeFormat(kText, 14);
    button_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    wrap_ = MakeFormat(kText, 14, DWRITE_FONT_WEIGHT_NORMAL, true);
    captionWrap_ = MakeFormat(kText, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
    mono_ = MakeFormat(L"Consolas", 12.5f);
    const std::wstring glyphs = ui::GlyphFamily(dwrite_.get());
    glyph_ = MakeFormat(glyphs.c_str(), 16);
    glyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    glyphBig_ = MakeFormat(glyphs.c_str(), 40);
    glyphBig_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  }

  ID2D1Factory* D2d() const { return d2d_.get(); }

  // The layout of `page` in a client area of `width` x `height` DIPs, the
  // page scrolled by `scroll` (clamped here).
  Layout Build(const UiContent& c, UiPage page, float width, float height, float& scroll, bool copied,
               bool selection) const {
    Layout l;
    BuildRail(l, c, page, height);

    const float pageWidth = std::min(kMaxPage, std::max(320.0f, width - kRail - 2 * kPadX));
    const float x0 = kRail + kPadX;
    const float x1 = x0 + pageWidth;
    l.viewTop = 0;
    l.viewBottom = height;
    const std::size_t first = l.items.size();

    float y = kTop;
    switch (page) {
      case UiPage::Overview: y = Overview(l, c, x0, x1, y); break;
      case UiPage::Protocol: y = Protocols(l, c, x0, x1, y); break;
      case UiPage::Subscription: y = Subscription(l, c, x0, x1, y); break;
      case UiPage::Apps: y = Apps(l, c, x0, x1, y); break;
      case UiPage::Logs: y = Logs(l, x0, x1, y, height, copied, selection); break;
      case UiPage::Settings: y = Settings(l, c, x0, x1, y); break;
    }
    l.contentHeight = y + kPadX;

    scroll = std::clamp(scroll, 0.0f, std::max(0.0f, l.contentHeight - height));
    l.scroll = scroll;
    for (std::size_t i = first; i < l.items.size(); ++i) {
      l.items[i].rect.top -= scroll;
      l.items[i].rect.bottom -= scroll;
    }
    if (l.logBox) {
      l.logBox->top -= scroll;
      l.logBox->bottom -= scroll;
    }
    return l;
  }

  // Draws a layout; `mark` is the app icon for the rail (may be null).
  void Draw(ID2D1RenderTarget* t, const Layout& l, const UiContent& c, const Interaction& in, ID2D1Bitmap* mark,
            const LogView& log) const {
    wil::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(t->CreateSolidColorBrush(Rgb(0, 0, 0), brush.put()))) {
      return;
    }
    Canvas k{t, brush.get()};
    const D2D1_SIZE_F size = t->GetSize();
    t->Clear(FromColorRef(ui::kWindowColor));
    k.Fill({0, 0, kRail, size.height}, FromColorRef(ui::kPanelColor));
    k.Line({kRail - 0.5f, 0}, {kRail - 0.5f, size.height}, Rgb(255, 255, 255, 0.06f));

    // The rail's header.
    if (mark != nullptr) {
      t->DrawBitmap(mark, {20, 20, 52, 52});
    }
    k.Text(L"Sovereign", brand_.get(), {62, 20, kRail - 12, 52}, FromColorRef(ui::kPrimaryText));

    bool clipped = false;
    for (std::size_t i = 0; i < l.items.size(); ++i) {
      const Item& it = l.items[i];
      if (it.scrolls && !clipped) {
        t->PushAxisAlignedClip({kRail, l.viewTop, size.width, l.viewBottom}, D2D1_ANTIALIAS_MODE_ALIASED);
        clipped = true;
      }
      if (it.rect.bottom < l.viewTop || it.rect.top > l.viewBottom) {
        continue;
      }
      const auto n = static_cast<int>(i);
      if (it.kind == Kind::LogBox && l.logBox) {
        DrawLog(k, *l.logBox, log);
      } else {
        DrawItem(k, it, c, n == in.hover, n == in.pressed);
      }
      if (n == in.focus && in.focusVisible) {
        t->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(it.rect, 2), kRadius, kRadius), SetColor(brush.get(), Rgb(255, 255, 255, 0.9f)), 2);
      }
    }
    if (clipped) {
      // The scroll position, when there is somewhere to scroll.
      const float view = l.viewBottom - l.viewTop;
      if (l.contentHeight > view + 1) {
        const float thumb = std::max(32.0f, view * view / l.contentHeight);
        const float top = l.viewTop + (view - thumb) * (l.scroll / (l.contentHeight - view));
        k.Round({size.width - 7, top + 4, size.width - 4, top + thumb - 4}, 1.5f, Rgb(255, 255, 255, 0.22f));
      }
      t->PopAxisAlignedClip();
    }
  }

  // The app icon as a bitmap of `target` at `dip` DIPs.
  wil::com_ptr<ID2D1Bitmap> Mark(ID2D1RenderTarget* target, float dip, float dpi) const {
    wil::com_ptr<ID2D1Bitmap> bitmap;
    if (!wic_) {
      return bitmap;
    }
    const wil::unique_hicon icon = LoadAppIcon(static_cast<int>(std::lround(dip * dpi / 96.0f)));
    wil::com_ptr<IWICBitmap> source;
    wil::com_ptr<IWICFormatConverter> converter;
    if (!icon || FAILED(wic_->CreateBitmapFromHICON(icon.get(), source.put())) ||
        FAILED(wic_->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) {
      return bitmap;
    }
    target->CreateBitmapFromWicBitmap(converter.get(), nullptr, bitmap.put());
    return bitmap;
  }

  IWICImagingFactory* Wic() const { return wic_.get(); }

 private:
  static constexpr const wchar_t* kDisplay = L"Segoe UI Variable Display";
  static constexpr const wchar_t* kText = L"Segoe UI Variable Text";

  // Drawing helpers over one reusable brush.
  struct Canvas {
    ID2D1RenderTarget* t;
    ID2D1SolidColorBrush* b;

    ID2D1Brush* Color(D2D1_COLOR_F c) const { return SetColor(b, c); }
    void Fill(D2D1_RECT_F r, D2D1_COLOR_F c) const { t->FillRectangle(r, Color(c)); }
    void Round(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) const {
      t->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Color(c));
    }
    void Outline(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width = 1) const {
      t->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(r, -width / 2), radius, radius), Color(c), width);
    }
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F z, D2D1_COLOR_F c, float width = 1) const {
      t->DrawLine(a, z, Color(c), width);
    }
    void Text(std::wstring_view s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) const {
      if (!s.empty()) {
        t->DrawText(s.data(), static_cast<UINT32>(s.size()), f, r, Color(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
      }
    }
  };

  static ID2D1Brush* SetColor(ID2D1SolidColorBrush* b, D2D1_COLOR_F c) {
    b->SetColor(c);
    return b;
  }

  wil::com_ptr<IDWriteTextFormat> MakeFormat(const wchar_t* family, float size,
                                             DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                                             bool wrap = false) const {
    wil::com_ptr<IDWriteTextFormat> f;
    THROW_IF_FAILED(dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"ru-ru", f.put()));
    if (wrap) {
      f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    } else {
      f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      if (ellipsis_) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trimming, ellipsis_.get());
      }
    }
    return f;
  }

  // The height `text` takes wrapped to `width` in `format`.
  float TextHeight(std::wstring_view text, IDWriteTextFormat* format, float width) const {
    wil::com_ptr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(dwrite_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, width, 10000,
                                         layout.put())) ||
        FAILED(layout->GetMetrics(&metrics))) {
      return 20;
    }
    return std::ceil(metrics.height);
  }

  /* ---- layout ---- */

  static void BuildRail(Layout& l, const UiContent& c, UiPage page, float height) {
    float y = 76;
    for (int p = 0; p < kUiPageCount; ++p) {
      const auto& info = kPages[static_cast<std::size_t>(p)];
      Item nav = Make(Kind::Nav, {10, y, kRail - 10, y + kNavRow}, info.name, info.glyph);
      nav.action = ItemAction::Page;
      nav.index = p;
      nav.checked = static_cast<int>(page) == p;
      nav.scrolls = false;
      l.items.push_back(std::move(nav));
      y += kNavRow + 4;
    }
    Item status = Make(Kind::RailStatus, {20, height - 64, kRail - 16, height - 18}, StateTitle(c));
    status.detail = c.display == Display::On ? L"↓ " + FormatRate(c.down) + L"   ↑ " + FormatRate(c.up) : std::wstring();
    status.scrolls = false;
    l.items.push_back(std::move(status));
  }

  float PageTitle(Layout& l, const wchar_t* title, const wchar_t* subtitle, float x0, float x1, float y) const {
    l.items.push_back(Make(Kind::Title, {x0, y, x1, y + 40}, title));
    y += 44;
    if (subtitle != nullptr) {
      const float h = TextHeight(subtitle, wrap_.get(), x1 - x0);
      l.items.push_back(Make(Kind::Wrap, {x0, y, x1, y + h}, subtitle));
      y += h;
    }
    return y + 20;
  }

  float Paragraph(Layout& l, Kind kind, const std::wstring& text, float x0, float x1, float y) const {
    const float indent = kind == Kind::ErrorText ? 28.0f : 0.0f;
    const float h = std::max(20.0f, TextHeight(text, wrap_.get(), x1 - x0 - indent));
    l.items.push_back(Make(kind, {x0, y, x1, y + h}, text, kind == Kind::ErrorText ? kGlyphWarning : nullptr));
    return y + h;
  }

  float Overview(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = PageTitle(l, L"Обзор", nullptr, x0, x1, y);

    // The state: the power button and what it means.
    const std::wstring detail = StateDetail(c);
    const float textLeft = x0 + 24 + 120 + 24;
    const float detailHeight = TextHeight(detail, wrap_.get(), x1 - 24 - textLeft);
    const float heroHeight = std::max(168.0f, 72 + detailHeight + 32);
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + heroHeight}));
    const float cy = y + heroHeight / 2;
    Item power = CommandItem(Kind::Power, {x0 + 24, cy - 60, x0 + 144, cy + 60}, {}, kGlyphPower, UiCommand::Toggle);
    power.checked = c.on;
    l.items.push_back(std::move(power));
    const float textTop = cy - (40 + 8 + detailHeight) / 2;
    Item state = Make(Kind::StateText, {textLeft, textTop, x1 - 24, textTop + 40 + 8 + detailHeight}, StateTitle(c));
    state.detail = detail;
    l.items.push_back(std::move(state));
    y += heroHeight + kGap;

    // Nothing to connect with yet: the way to fix that comes first.
    if (!c.hasSubscription && c.protocols.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 76}));
      l.items.push_back(Make(Kind::Text, {x0 + 20, y + 14, x1 - 260, y + 38}, L"Нет подписки"));
      l.items.push_back(Make(Kind::Muted, {x0 + 20, y + 38, x1 - 260, y + 62}, L"Скопируй ссылку на подписку в буфер обмена."));
      l.items.push_back(CommandItem(Kind::AccentButton, {x1 - 236, y + 21, x1 - 20, y + 21 + kButton},
                                    L"Вставить из буфера", kGlyphPaste, UiCommand::PasteSubscription));
      y += 76 + kGap;
    }

    // Speed and connections.
    const float cell = (x1 - x0 - 2 * kGap) / 3;
    const auto stat = [&](int n, const wchar_t* caption, std::wstring value) {
      const float left = x0 + static_cast<float>(n) * (cell + kGap);
      Item s = Make(Kind::Stat, {left, y, left + cell, y + 84}, caption);
      s.detail = std::move(value);
      s.index = n;
      l.items.push_back(std::move(s));
    };
    const bool on = c.display == Display::On;
    stat(0, L"Загрузка", on ? FormatRate(c.down) : L"—");
    stat(1, L"Отдача", on ? FormatRate(c.up) : L"—");
    stat(2, L"Соединения", on ? std::to_wstring(c.connections) : L"—");
    y += 84 + kGap;

    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 232}));
    l.items.push_back(Make(Kind::Heading, {x0 + 20, y + 12, x1 - 20, y + 40}, L"Скорость за 2 минуты"));
    l.items.push_back(Make(Kind::Graph, {x0 + 20, y + 48, x1 - 20, y + 216}));
    y += 232;

    return y;
  }

  float Protocols(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = PageTitle(l, L"Протокол", L"Через какой выход подписки идёт трафик. Выбор сохраняется и переживает обновление подписки, пока выход с таким именем в ней есть.",
                  x0, x1, y);
    if (c.protocols.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 64}));
      l.items.push_back(Make(Kind::Muted, {x0 + 20, y, x1 - 20, y + 64},
                             c.hasSubscription ? L"В конфиге подписки нет выбора протокола." : L"Нет подписки — нечего выбирать."));
      return y + 64;
    }
    // The latency test: a button and a line on how it went.
    Item test = CommandItem(Kind::Button, {x0, y, x0 + 236, y + kButton},
                            c.delaysTesting ? L"Проверяю…" : L"Проверить задержку", kGlyphStopwatch,
                            UiCommand::TestDelays);
    test.enabled = c.canTestDelays && !c.delaysTesting;
    l.items.push_back(std::move(test));
    std::wstring note = L"Запрос через каждый сервер; меньше — лучше.";
    if (!c.canTestDelays) {
      note = L"Задержку можно проверить, когда подключение включено.";
    } else if (!c.delayError.empty()) {
      note = L"Не удалось: " + c.delayError;
    }
    l.items.push_back(Make(Kind::Muted, {x0 + 252, y, x1, y + kButton}, std::move(note)));
    y += kButton + 12;

    const float h = static_cast<float>(c.protocols.size()) * kRow;
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
    for (std::size_t i = 0; i < c.protocols.size(); ++i) {
      const float top = y + static_cast<float>(i) * kRow;
      if (i > 0) {
        l.items.push_back(Make(Kind::Divider, {x0 + 16, top, x1 - 16, top + 1}));
      }
      Item choice = CommandItem(Kind::Choice, {x0 + 4, top + 4, x1 - 4, top + kRow - 4}, c.protocols[i], nullptr,
                                UiCommand::SetProtocol, static_cast<int>(i));
      choice.detail = c.protocols[i] == L"auto" ? L"лучший по задержке" : L"";
      choice.checked = static_cast<int>(i) == c.protocol;
      l.items.push_back(std::move(choice));
    }
    return y + h;
  }

  float Subscription(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = PageTitle(l, L"Подписка", L"Конфиг sing-box с сервера подписки. Скопируй ссылку (https://…) и нажми «Вставить» — Sovereign скачает конфиг и дальше будет обновлять его сам.",
                  x0, x1, y);
    std::wstring state;
    if (!c.hasSubscription) {
      state = L"нет";
    } else if (!c.subscriptionError.empty()) {
      state = L"ошибка — работает прежний конфиг";
    } else {
      state = c.subscription;
    }
    const std::vector<std::pair<const wchar_t*, std::wstring>> fields = {
        {L"Обновлена", state},
        {L"Сервер", c.subscriptionHost.empty() ? std::wstring(L"—") : c.subscriptionHost},
        {L"Интервал", c.updateHours > 0 ? std::format(L"каждые {} ч", c.updateHours) : std::wstring(L"—")},
    };
    const float h = static_cast<float>(fields.size()) * kRow;
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
    for (std::size_t i = 0; i < fields.size(); ++i) {
      const float top = y + static_cast<float>(i) * kRow;
      if (i > 0) {
        l.items.push_back(Make(Kind::Divider, {x0 + 16, top, x1 - 16, top + 1}));
      }
      Item field = Make(Kind::Field, {x0 + 20, top, x1 - 20, top + kRow}, fields[i].first);
      field.detail = fields[i].second;
      l.items.push_back(std::move(field));
    }
    y += h + kGap;

    if (!c.subscriptionError.empty()) {
      y = Paragraph(l, Kind::ErrorText, c.subscriptionError, x0, x1, y) + kGap;
    }

    l.items.push_back(CommandItem(Kind::AccentButton, {x0, y, x0 + 260, y + kButton}, L"Вставить ссылку из буфера",
                                  kGlyphPaste, UiCommand::PasteSubscription));
    Item refresh = CommandItem(Kind::Button, {x0 + 272, y, x0 + 452, y + kButton}, L"Обновить сейчас", kGlyphRefresh,
                               UiCommand::RefreshSubscription);
    refresh.enabled = c.hasSubscription;
    l.items.push_back(std::move(refresh));
    return y + kButton;
  }

  float Apps(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = PageTitle(l, L"Приложения", L"Какие программы идут через VPN. Имена — как у exe-файла, без пути.", x0, x1, y);
    const float mid = (x0 + x1) / 2;
    Item except = CommandItem(Kind::Segment, {x0, y, mid - 4, y + 36}, L"Все, кроме списка", nullptr,
                              UiCommand::SetAppsMode, 0);
    except.checked = !c.appsInclude;
    Item only = CommandItem(Kind::Segment, {mid + 4, y, x1, y + 36}, L"Только список", nullptr, UiCommand::SetAppsMode, 1);
    only.checked = c.appsInclude;
    l.items.push_back(std::move(except));
    l.items.push_back(std::move(only));
    y += 36 + 12;
    y = Paragraph(l, Kind::Wrap,
                  c.appsInclude ? L"Через VPN идут только программы из списка, остальной трафик — напрямую."
                                : L"Весь трафик идёт через VPN, кроме программ из списка — они ходят напрямую.",
                  x0, x1, y) +
        kGap;

    if (c.apps.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 64}));
      l.items.push_back(Make(Kind::Muted, {x0 + 20, y, x1 - 20, y + 64},
                             c.appsInclude ? L"Список пуст — через VPN не идёт ничего." : L"Список пуст — всё идёт через VPN."));
      y += 64;
    } else {
      const float h = static_cast<float>(c.apps.size()) * kRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t i = 0; i < c.apps.size(); ++i) {
        const float top = y + static_cast<float>(i) * kRow;
        if (i > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 16, top, x1 - 16, top + 1}));
        }
        l.items.push_back(Make(Kind::AppRow, {x0 + 8, top, x1 - 56, top + kRow}, c.apps[i], kGlyphProgram));
        const float bt = top + (kRow - kButton) / 2;
        Item remove = CommandItem(Kind::IconButton, {x1 - 12 - kButton, bt, x1 - 12, bt + kButton}, L"Убрать",
                                  kGlyphRemove, UiCommand::RemoveApp, static_cast<int>(i));
        l.items.push_back(std::move(remove));
      }
      y += h;
    }
    y += kGap;
    l.items.push_back(CommandItem(Kind::Button, {x0, y, x0 + 236, y + kButton}, L"Добавить из запущенных", kGlyphAdd,
                                  UiCommand::AddRunning));
    l.items.push_back(
        CommandItem(Kind::Button, {x0 + 248, y, x0 + 420, y + kButton}, L"Выбрать exe…", kGlyphFile, UiCommand::AddExe));
    return y + kButton;
  }

  float Logs(Layout& l, float x0, float x1, float y, float height, bool copied, bool selection) const {
    l.items.push_back(Make(Kind::Title, {x0, y, x1 - 240, y + 40}, L"Журнал"));
    std::wstring label = selection ? L"Копировать выделенное" : L"Копировать всё";
    if (copied) {
      label = L"Скопировано";
    }
    Item copy = Make(Kind::Button, {x1 - 230, y + 3, x1, y + 3 + kButton}, std::move(label), copied ? kGlyphCheck : kGlyphCopy);
    copy.action = ItemAction::CopyLogs;
    l.items.push_back(std::move(copy));
    y += 44 + 4;
    l.items.push_back(Make(Kind::Muted, {x0, y, x1, y + 20},
                           L"Выделение — мышью (с Shift — диапазон), Ctrl+A — всё, Ctrl+C — скопировать."));
    y += 20 + 10;
    // Fills the window: the lines scroll inside, the page doesn't.
    const float bottom = std::max(y + 120, height - kPadX);
    l.items.push_back(Make(Kind::Card, {x0, y, x1, bottom}));
    l.items.push_back(Make(Kind::LogBox, {x0, y, x1, bottom}));
    l.logBox = D2D1_RECT_F{x0 + 1, y + 8, x1 - 1, bottom - 8};
    return bottom - kPadX + 1;  // exactly the view: nothing to scroll
  }

  float Settings(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = PageTitle(l, L"Настройки", nullptr, x0, x1, y);

    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 72}));
    Item autostart = CommandItem(Kind::Switch, {x0 + 4, y + 4, x1 - 4, y + 68}, L"Запуск при входе в Windows", nullptr,
                                 UiCommand::ToggleAutostart);
    autostart.detail = L"Sovereign стартует в трее, окно не открывается.";
    autostart.checked = c.autostart;
    l.items.push_back(std::move(autostart));
    y += 72 + kGap;

    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 72}));
    l.items.push_back(Make(Kind::Text, {x0 + 20, y + 12, x1 - 180, y + 36}, L"Папка данных"));
    l.items.push_back(
        Make(Kind::Muted, {x0 + 20, y + 36, x1 - 180, y + 60}, L"%LOCALAPPDATA%\\Sovereign — настройки и конфиг"));
    l.items.push_back(CommandItem(Kind::Button, {x1 - 20 - 140, y + 19, x1 - 20, y + 19 + kButton}, L"Открыть",
                                  kGlyphFolder, UiCommand::OpenFolder));
    y += 72 + kGap;

    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 72}));
    l.items.push_back(Make(Kind::Text, {x0 + 20, y + 12, x1 - 20, y + 36}, L"О программе"));
    l.items.push_back(Make(Kind::Muted, {x0 + 20, y + 36, x1 - 20, y + 60}, L"Sovereign " + c.version));
    y += 72 + kGap + 8;

    l.items.push_back(CommandItem(Kind::DangerButton, {x0, y, x0 + 220, y + kButton}, L"Выйти из Sovereign", kGlyphExit,
                                  UiCommand::Exit));
    y += kButton + 10;
    return Paragraph(l, Kind::Wrap,
                     L"Трей закроется. Подключение этим не выключается — им управляет служба; при следующем "
                     L"запуске Sovereign продолжит с того же места.",
                     x0, x1, y);
  }

  /* ---- drawing ---- */

  void DrawItem(const Canvas& k, const Item& it, const UiContent& c, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    const D2D1_COLOR_F primary = FromColorRef(ui::kPrimaryText);
    const D2D1_COLOR_F secondary = FromColorRef(ui::kSecondaryText);
    const D2D1_COLOR_F accent = FromColorRef(ui::kAccent);
    const float hoverAlpha = pressed ? 0.04f : (hovered ? 0.08f : 0.0f);
    switch (it.kind) {
      case Kind::Nav: {
        if (it.checked || hovered) {
          k.Round(r, 6, Rgb(255, 255, 255, it.checked ? 0.07f : 0.04f));
        }
        if (it.checked) {
          const float mid = (r.top + r.bottom) / 2;
          k.Round({r.left, mid - 9, r.left + 3, mid + 9}, 1.5f, accent);
        }
        k.Text(it.glyph, glyph_.get(), {r.left + 10, r.top, r.left + 38, r.bottom}, it.checked ? accent : primary);
        k.Text(it.text, body_.get(), {r.left + 48, r.top, r.right - 8, r.bottom}, primary);
        break;
      }
      case Kind::RailStatus: {
        k.t->FillEllipse(D2D1::Ellipse({r.left + 5, r.top + 12}, 5, 5), k.Color(FromColorRef(c.statusDot)));
        k.Text(it.text, caption_.get(), {r.left + 18, r.top, r.right, r.top + 24}, primary);
        k.Text(it.detail, caption_.get(), {r.left + 18, r.top + 22, r.right, r.top + 46}, secondary);
        break;
      }
      case Kind::Card:
        k.Round(r, kRadius, FromColorRef(ui::kCardColor));
        k.Outline(r, kRadius, Rgb(255, 255, 255, 0.06f));
        break;
      case Kind::Divider: k.Line({r.left, r.top + 0.5f}, {r.right, r.top + 0.5f}, Rgb(255, 255, 255, 0.06f)); break;
      case Kind::Title: k.Text(it.text, title_.get(), r, primary); break;
      case Kind::Heading: k.Text(it.text, heading_.get(), r, primary); break;
      case Kind::Text: k.Text(it.text, body_.get(), r, primary); break;
      case Kind::Muted: k.Text(it.text, caption_.get(), r, secondary); break;
      case Kind::Wrap: k.Text(it.text, wrap_.get(), r, secondary); break;
      case Kind::ErrorText: {
        const D2D1_COLOR_F danger = FromColorRef(ui::kDanger);
        k.Text(it.glyph, glyph_.get(), {r.left, r.top, r.left + 20, r.top + 20}, danger);
        k.Text(it.text, wrap_.get(), {r.left + 28, r.top, r.right, r.bottom}, danger);
        break;
      }
      case Kind::Field:
        k.Text(it.text, body_.get(), {r.left, r.top, r.left + 160, r.bottom}, secondary);
        k.Text(it.detail, body_.get(), {r.left + 160, r.top, r.right, r.bottom}, primary);
        break;
      case Kind::Power: DrawPower(k, it, c, hovered, pressed); break;
      case Kind::StateText: {
        k.Text(it.text, state_.get(), {r.left, r.top, r.right, r.top + 40}, primary);
        k.Text(it.detail, wrap_.get(), {r.left, r.top + 48, r.right, r.bottom},
               c.display == Display::Error ? FromColorRef(ui::kDanger) : secondary);
        break;
      }
      case Kind::Stat: {
        k.Round(r, kRadius, FromColorRef(ui::kCardColor));
        k.Outline(r, kRadius, Rgb(255, 255, 255, 0.06f));
        D2D1_COLOR_F mark = secondary;
        if (it.index == 0) {
          mark = accent;
        } else if (it.index == 1) {
          mark = FromColorRef(ui::kUpload);
        }
        k.Round({r.left + 18, r.top + 22, r.left + 22, r.top + 34}, 2, mark);
        k.Text(it.text, caption_.get(), {r.left + 30, r.top + 16, r.right - 12, r.top + 40}, secondary);
        k.Text(it.detail, value_.get(), {r.left + 18, r.top + 40, r.right - 12, r.bottom - 12}, primary);
        break;
      }
      case Kind::Graph: DrawGraph(k, r, c); break;
      case Kind::Button:
      case Kind::AccentButton:
      case Kind::DangerButton: DrawButton(k, it, hovered, pressed); break;
      case Kind::IconButton:
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        k.Text(it.glyph, glyph_.get(), r, hovered ? primary : secondary);
        break;
      case Kind::Switch: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha * 0.6f));
        }
        const float right = r.right - 16;
        k.Text(it.text, body_.get(), {r.left + 16, r.top + 10, right - 60, r.top + 34}, primary);
        k.Text(it.detail, caption_.get(), {r.left + 16, r.top + 34, right - 60, r.top + 56}, secondary);
        DrawSwitch(k, {right - 40, (r.top + r.bottom) / 2 - 10, right, (r.top + r.bottom) / 2 + 10}, it.checked);
        break;
      }
      case Kind::Choice: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha * 0.6f));
        }
        const D2D1_POINT_2F dot{r.left + 26, (r.top + r.bottom) / 2};
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse(dot, 9, 9), k.Color(accent));
          k.t->FillEllipse(D2D1::Ellipse(dot, 4, 4), k.Color(Rgb(12, 20, 36)));
        } else {
          k.t->DrawEllipse(D2D1::Ellipse(dot, 8.5f, 8.5f), k.Color(secondary), 1.2f);
        }
        k.Text(it.text, body_.get(), {r.left + 50, r.top, r.right - 290, r.bottom}, primary);
        k.Text(it.detail, caption_.get(), {r.right - 280, r.top, r.right - 110, r.bottom}, secondary);
        if (it.index >= 0 && static_cast<std::size_t>(it.index) < c.delays.size()) {
          const UiDelay& delay = c.delays[static_cast<std::size_t>(it.index)];
          k.Text(DelayLabel(delay), delay_.get(), {r.right - 110, r.top, r.right - 16, r.bottom},
                 FromColorRef(ui::DelayColor(delay)));
        }
        break;
      }
      case Kind::Segment: {
        if (it.checked) {
          k.Round(r, 6, accent);
        } else {
          k.Round(r, 6, Rgb(255, 255, 255, 0.04f + hoverAlpha));
          k.Outline(r, 6, Rgb(255, 255, 255, 0.08f));
        }
        k.Text(it.text, button_.get(), r, it.checked ? Rgb(12, 20, 36) : primary);
        break;
      }
      case Kind::AppRow:
        k.Text(it.glyph, glyph_.get(), {r.left + 8, r.top, r.left + 36, r.bottom}, secondary);
        k.Text(it.text, body_.get(), {r.left + 48, r.top, r.right, r.bottom}, primary);
        break;
      case Kind::LogBox: break;  // DrawLog
    }
  }

  // The log's visible lines in `box`: time and level in their own colors,
  // selected lines highlighted, a thumb when there is more to scroll.
  void DrawLog(const Canvas& k, D2D1_RECT_F box, const LogView& log) const {
    const D2D1_COLOR_F primary = Rgb(222, 226, 234);
    const D2D1_COLOR_F secondary = FromColorRef(ui::kSecondaryText);
    if (log.lines.empty()) {
      k.Text(L"Пока пусто: служба ещё ничего не написала.", button_.get(), box, secondary);
      return;
    }
    k.t->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
    const float view = box.bottom - box.top;
    const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor(log.scroll / kLogLine)));
    for (std::size_t i = first; i < log.lines.size(); ++i) {
      const float top = box.top + static_cast<float>(i) * kLogLine - log.scroll;
      if (top > box.bottom) {
        break;
      }
      const D2D1_RECT_F row{box.left, top, box.right, top + kLogLine};
      if (log.Selected(static_cast<int>(i))) {
        k.Fill(row, FromColorRef(ui::kAccent, 0.22f));
      }
      // "05:07:30  ERROR  outbound/...": FormatLogLine's columns (main.cpp).
      const std::wstring_view line = log.lines[i];
      const float x = box.left + 14;
      if (line.size() > 17 && line[8] == L' ' && line[9] == L' ') {
        const std::wstring_view level = line.substr(10, 5);
        D2D1_COLOR_F levelColor = secondary;
        D2D1_COLOR_F message = primary;
        if (level.starts_with(L"ERROR") || level.starts_with(L"FATAL") || level.starts_with(L"PANIC")) {
          levelColor = FromColorRef(ui::kDanger);
          message = levelColor;
        } else if (level.starts_with(L"WARN")) {
          levelColor = FromColorRef(RGB(235, 180, 60));
        } else if (level.starts_with(L"DEBUG") || level.starts_with(L"TRACE")) {
          message = secondary;
        }
        k.Text(line.substr(0, 8), mono_.get(), {x, row.top, x + 70, row.bottom}, secondary);
        k.Text(level, mono_.get(), {x + 72, row.top, x + 124, row.bottom}, levelColor);
        k.Text(line.substr(17), mono_.get(), {x + 126, row.top, box.right - 14, row.bottom}, message);
      } else {
        k.Text(line, mono_.get(), {x, row.top, box.right - 14, row.bottom}, primary);
      }
    }
    if (log.Height() > view + 1) {
      const float thumb = std::max(32.0f, view * view / log.Height());
      const float top = box.top + (view - thumb) * (log.scroll / (log.Height() - view));
      k.Round({box.right - 7, top + 2, box.right - 4, top + thumb - 2}, 1.5f, Rgb(255, 255, 255, 0.22f));
    }
    k.t->PopAxisAlignedClip();
  }

  void DrawSwitch(const Canvas& k, D2D1_RECT_F track, bool on) const {
    const float mid = (track.top + track.bottom) / 2;
    if (on) {
      k.Round(track, 10, FromColorRef(ui::kAccent));
      k.t->FillEllipse(D2D1::Ellipse({track.right - 10, mid}, 6, 6), k.Color(Rgb(12, 20, 36)));
    } else {
      k.Outline(track, 10, FromColorRef(ui::kSecondaryText), 1.2f);
      k.t->FillEllipse(D2D1::Ellipse({track.left + 10, mid}, 5, 5), k.Color(FromColorRef(ui::kSecondaryText)));
    }
  }

  void DrawButton(const Canvas& k, const Item& it, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    D2D1_COLOR_F fill = Rgb(255, 255, 255, 0.06f);
    D2D1_COLOR_F ink = FromColorRef(ui::kPrimaryText);
    if (it.kind == Kind::AccentButton) {
      fill = FromColorRef(ui::kAccent, pressed ? 0.8f : (hovered ? 0.9f : 1.0f));
      ink = Rgb(12, 20, 36);
    } else if (it.kind == Kind::DangerButton) {
      fill = FromColorRef(ui::kDanger, pressed ? 0.12f : (hovered ? 0.24f : 0.16f));
      ink = FromColorRef(ui::kDanger);
    } else if (hovered) {
      fill = Rgb(255, 255, 255, pressed ? 0.05f : 0.10f);
    }
    if (!it.enabled) {
      fill = Rgb(255, 255, 255, 0.03f);
      ink = FromColorRef(ui::kSecondaryText, 0.5f);
    }
    k.Round(r, 6, fill);
    if (it.kind == Kind::Button) {
      k.Outline(r, 6, Rgb(255, 255, 255, 0.07f));
    }
    // Glyph and text centered together.
    wil::com_ptr<IDWriteTextLayout> layout;
    float textWidth = 0;
    if (SUCCEEDED(dwrite_->CreateTextLayout(it.text.data(), static_cast<UINT32>(it.text.size()), button_.get(),
                                            r.right - r.left, r.bottom - r.top, layout.put()))) {
      DWRITE_TEXT_METRICS m{};
      if (SUCCEEDED(layout->GetMetrics(&m))) {
        textWidth = m.width;
      }
    }
    const float glyphWidth = it.glyph.empty() ? 0.0f : 26.0f;
    const float left = (r.left + r.right - textWidth - glyphWidth) / 2;
    if (!it.glyph.empty()) {
      k.Text(it.glyph, glyph_.get(), {left, r.top, left + 18, r.bottom}, ink);
    }
    const D2D1_RECT_F textBox{left + glyphWidth, r.top, left + glyphWidth + textWidth + 2, r.bottom};
    k.Text(it.text, body_.get(), textBox, ink);
  }

  void DrawPower(const Canvas& k, const Item& it, const UiContent& c, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    const D2D1_POINT_2F center{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    const float radius = (r.right - r.left) / 2;
    D2D1_COLOR_F ring = FromColorRef(ui::kSecondaryText, 0.5f);
    switch (c.display) {
      case Display::On: ring = FromColorRef(ui::kAccent); break;
      case Display::Starting: ring = FromColorRef(RGB(235, 165, 0)); break;
      case Display::Error: ring = FromColorRef(ui::kDanger); break;
      case Display::ServiceDown:
      case Display::Off: break;
    }
    if (it.checked) {
      // On: a soft halo, the accent disc, a dark glyph.
      k.t->FillEllipse(D2D1::Ellipse(center, radius, radius), k.Color(FromColorRef(ui::kAccent, 0.12f)));
      const float inner = radius - 10;
      k.t->FillEllipse(D2D1::Ellipse(center, inner, inner),
                       k.Color(FromColorRef(ui::kAccent, pressed ? 0.8f : (hovered ? 0.92f : 1.0f))));
      if (c.display != Display::On) {
        k.t->DrawEllipse(D2D1::Ellipse(center, radius - 2, radius - 2), k.Color(ring), 3);
      }
      k.Text(it.glyph, glyphBig_.get(), r, Rgb(12, 20, 36));
    } else {
      const float inner = radius - 10;
      k.t->FillEllipse(D2D1::Ellipse(center, inner, inner),
                       k.Color(Rgb(255, 255, 255, pressed ? 0.05f : (hovered ? 0.10f : 0.06f))));
      k.t->DrawEllipse(D2D1::Ellipse(center, inner, inner), k.Color(ring), 2);
      k.Text(it.glyph, glyphBig_.get(), r, FromColorRef(ui::kPrimaryText));
    }
  }

  void DrawGraph(const Canvas& k, D2D1_RECT_F r, const UiContent& c) const {
    const D2D1_COLOR_F grid = Rgb(255, 255, 255, 0.06f);
    const D2D1_COLOR_F secondary = FromColorRef(ui::kSecondaryText);
    // The legend, in the heading's row.
    const D2D1_RECT_F legend{r.right - 230, r.top - 36, r.right, r.top - 8};
    k.Round({legend.left, legend.top + 12, legend.left + 10, legend.top + 16}, 2, FromColorRef(ui::kAccent));
    k.Text(L"загрузка", caption_.get(), {legend.left + 16, legend.top, legend.left + 110, legend.bottom}, secondary);
    k.Round({legend.left + 120, legend.top + 12, legend.left + 130, legend.top + 16}, 2, FromColorRef(ui::kUpload));
    k.Text(L"отдача", caption_.get(), {legend.left + 136, legend.top, legend.right, legend.bottom}, secondary);

    float peak = 0;
    for (const auto& [down, up] : c.history) {
      peak = std::max({peak, down, up});
    }
    // The scale's top: a power of two from 4 KB/s, so its quarters read round.
    float top = 4096;
    while (top < peak * 1.1f) {
      top *= 2;
    }
    const bool idle = peak <= 0;
    const float left = r.left + 64;
    for (int i = 0; i <= 4; ++i) {
      const float y = r.bottom - (r.bottom - r.top) * static_cast<float>(i) / 4.0f;
      k.Line({left, std::round(y) + 0.5f}, {r.right, std::round(y) + 0.5f}, grid);
      if (i > 0 && !idle) {
        k.Text(FormatRate(static_cast<double>(top) * i / 4), caption_.get(), {r.left, y - 10, left - 8, y + 10},
               secondary);
      }
    }
    if (idle) {
      k.Text(c.display == Display::On ? L"трафика нет" : L"нет данных — подключение выключено", button_.get(),
             {left, r.top, r.right, r.bottom}, secondary);
      return;
    }
    const float step = (r.right - left) / static_cast<float>(kHistory - 1);
    const std::size_t count = std::min(c.history.size(), kHistory);
    const std::size_t skip = c.history.size() - count;
    const auto x = [&](std::size_t i) { return r.right - step * static_cast<float>(count - 1 - i); };
    const auto y = [&](float v) { return r.bottom - (r.bottom - r.top) * std::min(v / top, 1.0f); };

    const auto series = [&](bool down, D2D1_COLOR_F color, bool fill) {
      wil::com_ptr<ID2D1PathGeometry> path;
      wil::com_ptr<ID2D1GeometrySink> sink;
      if (FAILED(d2d_->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) {
        return;
      }
      const auto value = [&](std::size_t i) {
        const auto& p = c.history[skip + i];
        return down ? p.first : p.second;
      };
      sink->BeginFigure(fill ? D2D1::Point2F(x(0), r.bottom) : D2D1::Point2F(x(0), y(value(0))),
                        fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
      for (std::size_t i = fill ? 0 : 1; i < count; ++i) {
        sink->AddLine(D2D1::Point2F(x(i), y(value(i))));
      }
      if (fill) {
        sink->AddLine(D2D1::Point2F(x(count - 1), r.bottom));
      }
      sink->EndFigure(fill ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
      if (FAILED(sink->Close())) {
        return;
      }
      if (fill) {
        k.t->FillGeometry(path.get(), k.Color(color));
      } else {
        k.t->DrawGeometry(path.get(), k.Color(color), 1.6f);
      }
    };
    series(true, FromColorRef(ui::kAccent, 0.16f), true);
    series(false, FromColorRef(ui::kUpload), false);
    series(true, FromColorRef(ui::kAccent), false);
  }

  wil::com_ptr<ID2D1Factory> d2d_;
  wil::com_ptr<IDWriteFactory> dwrite_;
  wil::com_ptr<IWICImagingFactory> wic_;
  wil::com_ptr<IDWriteInlineObject> ellipsis_;
  wil::com_ptr<IDWriteTextFormat> title_;
  wil::com_ptr<IDWriteTextFormat> state_;
  wil::com_ptr<IDWriteTextFormat> value_;
  wil::com_ptr<IDWriteTextFormat> delay_;
  wil::com_ptr<IDWriteTextFormat> brand_;
  wil::com_ptr<IDWriteTextFormat> heading_;
  wil::com_ptr<IDWriteTextFormat> body_;
  wil::com_ptr<IDWriteTextFormat> caption_;
  wil::com_ptr<IDWriteTextFormat> button_;
  wil::com_ptr<IDWriteTextFormat> wrap_;
  wil::com_ptr<IDWriteTextFormat> captionWrap_;
  wil::com_ptr<IDWriteTextFormat> mono_;
  wil::com_ptr<IDWriteTextFormat> glyph_;
  wil::com_ptr<IDWriteTextFormat> glyphBig_;
};

int Scale(float dip, UINT dpi) { return static_cast<int>(std::lround(dip * static_cast<float>(dpi) / 96.0f)); }

// `text` onto the clipboard; false if the clipboard wouldn't take it.
bool CopyToClipboard(HWND owner, const std::wstring& text) {
  if (!OpenClipboard(owner)) {
    return false;
  }
  bool copied = false;
  if (EmptyClipboard()) {
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    wil::unique_hglobal memory(GlobalAlloc(GMEM_MOVEABLE, bytes));
    if (memory) {
      if (void* locked = GlobalLock(memory.get()); locked != nullptr) {
        std::memcpy(locked, text.c_str(), bytes);
        GlobalUnlock(memory.get());
        if (SetClipboardData(CF_UNICODETEXT, memory.get()) != nullptr) {
          memory.release();  // the clipboard owns it now
          copied = true;
        }
      }
    }
  }
  CloseClipboard();
  return copied;
}

}  // namespace

struct MainWindow::Impl {
  HINSTANCE instance;
  CommandHandler onCommand;
  Painter painter;
  wil::unique_hwnd window;
  HWND hwnd = nullptr;  // window's handle, known from WM_NCCREATE on (window is set when creation returns)
  wil::unique_hicon bigIcon;
  wil::unique_hicon smallIcon;

  UiContent content;
  UiPage page = UiPage::Overview;
  float scroll = 0;
  Layout layout;
  Interaction in;
  LogView log;
  bool selecting = false;      // dragging a selection over the log
  std::optional<POINT> mouse;  // the cursor over the client area, in pixels
  bool copied = false;
  bool placed = false;  // sized and centered once, on the first show

  wil::com_ptr<ID2D1HwndRenderTarget> target;
  wil::com_ptr<ID2D1Bitmap> mark;  // tied to the target

  Impl(HINSTANCE inst, CommandHandler handler) : instance(inst), onCommand(std::move(handler)) {
    WNDCLASSW wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Impl::WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW, see flyout.cpp
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);  // already registered is fine
    window.reset(CreateWindowExW(0, kClassName, L"Sovereign", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, this));
    THROW_LAST_ERROR_IF(!window);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    const COLORREF caption = ui::kPanelColor;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof caption);  // Windows 11; ignored on 10
    const UINT dpi = GetDpiForWindow(hwnd);
    bigIcon = LoadAppIcon(GetSystemMetricsForDpi(SM_CXICON, dpi));
    smallIcon = LoadAppIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon.get()));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon.get()));
  }

  ~Impl() {
    // The window goes before the members its procedure uses.
    window.reset();
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
      static_cast<Impl*>(create->lpCreateParams)->hwnd = hwnd;
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));  // NOLINT(performance-no-int-to-ptr)
    if (message == WM_NCDESTROY) {
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      if (self != nullptr) {
        self->hwnd = nullptr;
      }
    }
    return self != nullptr ? self->Handle(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
  }

  UINT Dpi() const { return GetDpiForWindow(hwnd); }
  float ToDip(int px) const { return static_cast<float>(px) * 96.0f / static_cast<float>(Dpi()); }

  D2D1_SIZE_F ClientDip() const {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return {ToDip(rc.right), ToDip(rc.bottom)};
  }

  int HitAt(POINT px) const {
    const float x = ToDip(px.x);
    const float y = ToDip(px.y);
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
      const Item& item = layout.items[i];
      if (!Interactive(item) || !Contains(item.rect, x, y)) {
        continue;
      }
      if (item.scrolls && (y < layout.viewTop || y >= layout.viewBottom || x < kRail)) {
        continue;
      }
      return static_cast<int>(i);
    }
    return -1;
  }

  bool InLog(POINT px) const { return layout.logBox && Contains(*layout.logBox, ToDip(px.x), ToDip(px.y)); }

  // The log line under a point, clamped to the lines there are; -1 if none.
  int LogLineAt(POINT px) const {
    if (!layout.logBox || log.lines.empty()) {
      return -1;
    }
    const float y = ToDip(px.y) - layout.logBox->top + log.scroll;
    const auto line = static_cast<int>(std::floor(y / kLogLine));
    return std::clamp(line, 0, static_cast<int>(log.lines.size()) - 1);
  }

  // The log's scroll within its lines, or at the bottom while following.
  void ClampLog() {
    if (!layout.logBox) {
      return;
    }
    const float view = layout.logBox->bottom - layout.logBox->top;
    const float max = std::max(0.0f, log.Height() - view);
    log.scroll = log.follow ? max : std::clamp(log.scroll, 0.0f, max);
  }

  void ScrollLog(float dip) {
    if (!layout.logBox) {
      return;
    }
    const float view = layout.logBox->bottom - layout.logBox->top;
    const float max = std::max(0.0f, log.Height() - view);
    log.scroll = std::clamp(log.scroll + dip, 0.0f, max);
    log.follow = log.scroll >= max - 1;
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  void Relayout() {
    const D2D1_SIZE_F size = ClientDip();
    layout = painter.Build(content, page, size.width, size.height, scroll, copied, log.HasSelection());
    ClampLog();
    in.hover = mouse ? HitAt(*mouse) : -1;
    if (in.focus >= static_cast<int>(layout.items.size()) ||
        (in.focus >= 0 && !Interactive(layout.items[static_cast<std::size_t>(in.focus)]))) {
      in.focus = -1;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  void Go(UiPage p) {
    if (p != page) {
      page = p;
      scroll = 0;
      in.focus = -1;
      in.pressed = -1;
    }
    Relayout();
  }

  void Activate(const Item& item) {
    switch (item.action) {
      case ItemAction::None: return;
      case ItemAction::Page: Go(static_cast<UiPage>(item.index)); return;
      case ItemAction::CopyLogs: CopyLogs(); return;
      case ItemAction::Command: break;
    }
    UiArgs args;
    args.index = item.index;
    args.owner = hwnd;
    const float scale = static_cast<float>(Dpi()) / 96.0f;
    POINT anchor{static_cast<LONG>(item.rect.left * scale), static_cast<LONG>(item.rect.bottom * scale)};
    ClientToScreen(hwnd, &anchor);
    args.anchor = anchor;
    onCommand(item.command, args);
  }

  // The selected log lines, or all of them, onto the clipboard.
  void CopyLogs() {
    std::wstring text;
    for (std::size_t i = 0; i < log.lines.size(); ++i) {
      if (!log.HasSelection() || log.Selected(static_cast<int>(i))) {
        text += log.lines[i];
        text += L"\r\n";
      }
    }
    if (CopyToClipboard(hwnd, text)) {
      copied = true;
      SetTimer(hwnd, kCopiedTimer, kCopiedMs, nullptr);
      Relayout();
    }
  }

  void AddLogs(const std::vector<std::wstring>& lines, bool replace) {
    if (replace) {
      log.lines.clear();
      log.anchor = log.caret = -1;
      log.follow = true;
    }
    for (const std::wstring& line : lines) {
      log.lines.push_back(line);
    }
    if (log.lines.size() > kLogKeep) {
      // The oldest go; what the user looks at and selected stays in place.
      const std::size_t cut = log.lines.size() - kLogKeep;
      log.lines.erase(log.lines.begin(), log.lines.begin() + static_cast<std::ptrdiff_t>(cut));
      const auto shift = static_cast<int>(cut);
      log.scroll = std::max(0.0f, log.scroll - static_cast<float>(cut) * kLogLine);
      if (log.HasSelection()) {
        log.anchor = std::max(log.anchor - shift, 0);
        log.caret = std::max(log.caret - shift, 0);
      }
    }
    ClampLog();
    if (page == UiPage::Logs && IsWindowVisible(hwnd)) {
      Relayout();
    }
  }

  // Tab order: the interactive items as laid out, rail first.
  void MoveFocus(bool back) {
    std::vector<int> order;
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
      if (Interactive(layout.items[i])) {
        order.push_back(static_cast<int>(i));
      }
    }
    if (order.empty()) {
      return;
    }
    auto at = std::find(order.begin(), order.end(), in.focus);
    if (at == order.end()) {
      in.focus = back ? order.back() : order.front();
    } else if (back) {
      in.focus = at == order.begin() ? order.back() : *(at - 1);
    } else {
      in.focus = (at + 1) == order.end() ? order.front() : *(at + 1);
    }
    in.focusVisible = true;
    ScrollIntoView(layout.items[static_cast<std::size_t>(in.focus)]);
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  void ScrollIntoView(const Item& item) {
    if (!item.scrolls) {
      return;
    }
    const float margin = 16;
    if (item.rect.top < layout.viewTop + margin) {
      scroll -= layout.viewTop + margin - item.rect.top;
    } else if (item.rect.bottom > layout.viewBottom - margin) {
      scroll += item.rect.bottom - (layout.viewBottom - margin);
    } else {
      return;
    }
    const int focus = in.focus;
    Relayout();
    in.focus = focus;
  }

  void ScrollBy(float dip) {
    scroll += dip;
    Relayout();
  }

  LRESULT Handle(HWND w, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
      case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(w, &ps);
        Paint();
        EndPaint(w, &ps);
        return 0;
      }
      case WM_ERASEBKGND: return 1;  // Direct2D paints everything
      case WM_SIZE:
        if (target) {
          target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        Relayout();
        return 0;
      case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);  // NOLINT(performance-no-int-to-ptr)
        const UINT dpi = GetDpiForWindow(w);
        info->ptMinTrackSize = {Scale(kMinWidth, dpi), Scale(kMinHeight, dpi)};
        return 0;
      }
      case WM_DPICHANGED: {
        // The system's suggestion keeps the window the same physical size.
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);  // NOLINT(performance-no-int-to-ptr)
        target.reset();
        mark.reset();
        SetWindowPos(w, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Relayout();
        return 0;
      }
      case WM_MOUSEMOVE: {
        const POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        mouse = at;
        if (selecting && layout.logBox) {
          const D2D1_RECT_F box = *layout.logBox;
          if (const int line = LogLineAt(at); line >= 0 && line != log.caret) {
            log.caret = line;
            // Past the box's edge: the lines follow the mouse.
            if (ToDip(at.y) < box.top) {
              ScrollLog(-kLogLine);
            } else if (ToDip(at.y) > box.bottom) {
              ScrollLog(kLogLine);
            }
            InvalidateRect(w, nullptr, FALSE);
          }
          return 0;
        }
        const int h = HitAt(at);
        if (h != in.hover) {
          in.hover = h;
          InvalidateRect(w, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, w, 0};
        TrackMouseEvent(&tme);
        return 0;
      }
      case WM_MOUSELEAVE:
        mouse.reset();
        in.hover = -1;
        InvalidateRect(w, nullptr, FALSE);
        return 0;
      case WM_LBUTTONDOWN: {
        const POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetFocus(w);
        SetCapture(w);
        in.focusVisible = false;
        if (InLog(at)) {
          const int line = LogLineAt(at);
          if ((wParam & MK_SHIFT) != 0 && log.HasSelection() && line >= 0) {
            log.caret = line;
          } else {
            log.anchor = log.caret = line;
          }
          selecting = line >= 0;
          Relayout();  // the copy button's label
          return 0;
        }
        in.pressed = HitAt(at);
        InvalidateRect(w, nullptr, FALSE);
        return 0;
      }
      case WM_LBUTTONUP: {
        ReleaseCapture();
        if (std::exchange(selecting, false)) {
          return 0;
        }
        const int h = HitAt({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        const int pressed = std::exchange(in.pressed, -1);
        InvalidateRect(w, nullptr, FALSE);
        if (h >= 0 && h == pressed) {
          in.focus = h;
          const Item item = layout.items[static_cast<std::size_t>(h)];  // a copy: the command may relayout
          Activate(item);
        }
        return 0;
      }
      case WM_CAPTURECHANGED:
        selecting = false;
        return 0;
      case WM_MOUSEWHEEL: {
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(w, &at);
        const float step = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        if (InLog(at)) {
          ScrollLog(step * 3 * kLogLine);
        } else if (ToDip(at.x) >= kRail) {
          ScrollBy(step * kWheelStep);
        }
        return 0;
      }
      case WM_KEYDOWN: return Key(wParam) ? 0 : DefWindowProcW(w, message, wParam, lParam);
      case WM_TIMER:
        if (wParam == kCopiedTimer) {
          KillTimer(w, kCopiedTimer);
          copied = false;
          Relayout();
        }
        return 0;
      case WM_CLOSE:
        // Only the view closes: the tray keeps running.
        ShowWindow(w, SW_HIDE);
        return 0;
      default:
        return DefWindowProcW(w, message, wParam, lParam);
    }
  }

  bool Key(WPARAM key) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl && key >= '1' && key < '1' + kUiPageCount) {
      Go(static_cast<UiPage>(key - '1'));
      return true;
    }
    const bool onLog = page == UiPage::Logs && layout.logBox;
    if (onLog && ctrl && key == 'A' && !log.lines.empty()) {
      log.anchor = 0;
      log.caret = static_cast<int>(log.lines.size()) - 1;
      Relayout();
      return true;
    }
    if (onLog && ctrl && key == 'C') {
      CopyLogs();
      return true;
    }
    if (onLog && key == VK_ESCAPE && log.HasSelection()) {
      log.anchor = log.caret = -1;
      Relayout();
      return true;
    }
    const float screen = onLog ? layout.logBox->bottom - layout.logBox->top - kLogLine : ClientDip().height * 0.8f;
    const auto scrollBy = [&](float dip) {
      if (onLog) {
        ScrollLog(dip);
      } else {
        ScrollBy(dip);
      }
    };
    const float total = onLog ? log.Height() : layout.contentHeight;
    switch (key) {
      case VK_TAB: MoveFocus(shift); return true;
      case VK_RETURN:
      case VK_SPACE:
        if (in.focus >= 0 && in.focus < static_cast<int>(layout.items.size())) {
          const Item item = layout.items[static_cast<std::size_t>(in.focus)];
          Activate(item);
        }
        return true;
      case VK_DOWN: scrollBy(onLog ? kLogLine : kWheelStep / 2); return true;
      case VK_UP: scrollBy(onLog ? -kLogLine : -kWheelStep / 2); return true;
      case VK_NEXT: scrollBy(screen); return true;
      case VK_PRIOR: scrollBy(-screen); return true;
      case VK_HOME: scrollBy(-total); return true;
      case VK_END: scrollBy(total); return true;
      default: return false;
    }
  }

  void Paint() {
    if (!target) {
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const auto dpi = static_cast<float>(Dpi());
      const D2D1_RENDER_TARGET_PROPERTIES props =
          D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi);
      if (FAILED(painter.D2d()->CreateHwndRenderTarget(
              props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), target.put()))) {
        return;
      }
      target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
      mark = painter.Mark(target.get(), 32, dpi);
    }
    target->BeginDraw();
    painter.Draw(target.get(), layout, content, in, mark.get(), log);
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
      target.reset();
      mark.reset();
    }
  }

  // Sized for the monitor under the cursor and centered in its work area.
  void Place() {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(monitor, &mi);
    UINT dpiX = 96;
    UINT dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const RECT work = mi.rcWork;
    const int w = std::min(Scale(kDefaultWidth, dpiX), static_cast<int>(work.right - work.left));
    const int h = std::min(Scale(kDefaultHeight, dpiX), static_cast<int>(work.bottom - work.top));
    SetWindowPos(hwnd, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  }
};

MainWindow::MainWindow(HINSTANCE instance, CommandHandler onCommand)
    : impl_(std::make_unique<Impl>(instance, std::move(onCommand))) {}

MainWindow::~MainWindow() = default;

void MainWindow::Show(UiPage page) {
  Impl& m = *impl_;
  if (!m.placed) {
    m.placed = true;
    m.Place();
  }
  m.Go(page);
  ShowWindow(m.hwnd, IsIconic(m.hwnd) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(m.hwnd);
}

void MainWindow::Hide() { ShowWindow(impl_->hwnd, SW_HIDE); }

bool MainWindow::IsVisible() const { return impl_->hwnd != nullptr && IsWindowVisible(impl_->hwnd) != FALSE; }

void MainWindow::Update(const UiContent& content) {
  impl_->content = content;
  if (IsVisible()) {
    impl_->Relayout();
  }
}

void MainWindow::SetLogs(const std::vector<std::wstring>& lines) { impl_->AddLogs(lines, true); }

void MainWindow::AppendLogs(const std::vector<std::wstring>& lines) {
  if (!lines.empty()) {
    impl_->AddLogs(lines, false);
  }
}

void RenderMainWindowSnapshot(const UiContent& content, UiPage page, const std::vector<std::wstring>& logs, UINT width,
                              UINT height, float dpi, const std::wstring& pngPath) {
  const Painter painter;
  IWICImagingFactory* wic = painter.Wic();
  THROW_HR_IF_NULL(E_NOINTERFACE, wic);
  wil::com_ptr<IWICBitmap> bitmap;
  THROW_IF_FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.put()));
  wil::com_ptr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi,
      dpi);
  THROW_IF_FAILED(painter.D2d()->CreateWicBitmapRenderTarget(bitmap.get(), props, target.put()));
  target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

  LogView log;
  log.lines.assign(logs.begin(), logs.end());
  if (log.lines.size() > 2) {
    // Shows what a selection looks like.
    log.anchor = 1;
    log.caret = 2;
  }
  float scroll = 0;
  const float scale = 96.0f / dpi;
  const Layout layout = painter.Build(content, page, static_cast<float>(width) * scale,
                                      static_cast<float>(height) * scale, scroll, false, log.HasSelection());
  const wil::com_ptr<ID2D1Bitmap> mark = painter.Mark(target.get(), 32, dpi);
  target->BeginDraw();
  painter.Draw(target.get(), layout, content, Interaction{}, mark.get(), log);
  THROW_IF_FAILED(target->EndDraw());

  wil::com_ptr<IWICStream> stream;
  THROW_IF_FAILED(wic->CreateStream(stream.put()));
  THROW_IF_FAILED(stream->InitializeFromFilename(pngPath.c_str(), GENERIC_WRITE));
  wil::com_ptr<IWICBitmapEncoder> encoder;
  THROW_IF_FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
  THROW_IF_FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
  wil::com_ptr<IWICBitmapFrameEncode> frame;
  THROW_IF_FAILED(encoder->CreateNewFrame(frame.put(), nullptr));
  THROW_IF_FAILED(frame->Initialize(nullptr));
  THROW_IF_FAILED(frame->SetSize(width, height));
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  THROW_IF_FAILED(frame->SetPixelFormat(&format));
  THROW_IF_FAILED(frame->WriteSource(bitmap.get(), nullptr));
  THROW_IF_FAILED(frame->Commit());
  THROW_IF_FAILED(encoder->Commit());
}

}  // namespace sovereign::tray
