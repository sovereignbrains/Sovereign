#include "main_window.h"

#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "flags.h"
#include "icons.h"
#include "resource.h"
#include "ui_style.h"

namespace sovereign::tray {

namespace {

using ui::FromColorRef;
using ui::Rgb;

constexpr wchar_t kClassName[] = L"SovereignMainWindow";

// Layout, in DIPs (1/96 inch): render targets work in DIPs at the window's
// DPI, so nothing below is scaled by hand.
constexpr float kDefaultWidth = 400;
constexpr float kDefaultHeight = 620;
constexpr float kMinWidth = 360;
constexpr float kMinHeight = 440;
constexpr float kPad = 16;         // the page's margins
constexpr float kMaxPage = 560;    // wider windows keep the page at this, centered
constexpr float kRow = 44;         // a list row
constexpr float kProfileRow = 54;  // a configuration's row: its name and its state
constexpr float kServerRow = 60;   // a server on a configuration's page: its name, protocol and switch
constexpr float kButton = 34;
constexpr float kGap = 12;         // between cards
constexpr float kRadius = 8;
constexpr float kWheelStep = 64;
constexpr float kPower = 60;       // the on/off button's diameter
constexpr float kBar = 148;        // the overview's bottom bar: the switch, the server, the exit
constexpr float kFlagW = 20;       // a flag in the bar, 4:3
constexpr float kFlagH = 15;
constexpr float kTile = 76;        // a tile on the overview
constexpr float kAppIcon = 20;     // a program's icon in the per-app list
constexpr ULONGLONG kToggleGraceMs = 400;  // a tray click right after the window lost focus hides it

// The log page: a line's height, and how many lines it keeps (the tray keeps
// no more either).
constexpr float kLogLine = 20;
constexpr std::size_t kLogKeep = 5000;

// The log's right-click menu.
// The edit box typed in lost the focus: keep what's in it (posted - the box
// can't be destroyed inside its own message).
constexpr UINT kEndEditMessage = WM_APP + 41;

constexpr UINT kLogMenuCopy = 1;
constexpr UINT kLogMenuCopyAll = 2;
constexpr UINT kLogMenuSelectAll = 3;
constexpr UINT kLogMenuSave = 4;
constexpr UINT kLogMenuClear = 5;

// Segoe Fluent Icons (Windows 11; the same code points in Segoe MDL2 Assets).
constexpr const wchar_t* kGlyphSync = L"\xE895";
constexpr const wchar_t* kGlyphBack = L"\xE72B";
constexpr const wchar_t* kGlyphChevron = L"\xE76C";
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
constexpr const wchar_t* kGlyphDownload = L"\xE896";
constexpr const wchar_t* kGlyphUndo = L"\xE7A7";
constexpr const wchar_t* kGlyphSave = L"\xE74E";
constexpr const wchar_t* kGlyphPause = L"\xE769";
constexpr const wchar_t* kGlyphPlay = L"\xE768";
constexpr const wchar_t* kGlyphFilter = L"\xE71C";
constexpr const wchar_t* kGlyphQrCode = L"\xED14";
constexpr const wchar_t* kGlyphDelete = L"\xE74D";
constexpr const wchar_t* kGlyphRename = L"\xE8AC";
constexpr const wchar_t* kGlyphClock = L"\xE917";

enum class Kind : std::uint8_t {
  Card,          // a rounded panel behind other items
  Divider,       // a line across a card
  Title,         // the page's title
  Heading,       // a card's title
  Text,          // one line
  Muted,         // one line, secondary
  Wrap,          // wrapped paragraph, secondary
  Caption,       // wrapped paragraph, secondary, small
  ErrorText,     // wrapped paragraph with a warning glyph, red
  Field,         // a label on the left, a value on the right
  Power,         // the overview's on/off button
  Tile,          // a square-ish button on the overview: glyph, title, a line; `checked`: a dot
  Bar,           // the overview's bottom panel
  ServerLink,    // the bar's server: its name and latency, centered, opens the servers
  ExitLine,      // the bar's exit: the country's flag (detail: its code) and the address or the country
  Banner,        // a one-line call to action in the accent color
  Button,        // a normal button
  AccentButton,  // the page's main action
  DangerButton,  // quit
  IconButton,    // a square button with a glyph; `checked`: a dot on it (something new there)
  Switch,        // a row with a toggle switch
  Choice,        // a pickable row with a radio mark
  ProfileRow,    // a configuration: name, its state under it (`warn`: in red), dimmed when off (!`checked`)
  Toggle,        // just a switch, no text (`checked`: on)
  Segment,       // half of a two-way switch
  Chip,          // an on/off filter: tinted when on
  AppRow,        // a program in the per-app list
  LogBox,        // the log's lines
};

// EditName: the configuration `index`'s name, typed over the page's title.
enum class ItemAction : std::uint8_t { None, Command, Page, OpenProfile, EditName, ToggleLevel, PauseLogs, SaveLogs };

struct Item {
  Kind kind = Kind::Text;
  D2D1_RECT_F rect{};
  std::wstring_view glyph;  // empty: none (never a null pointer - /analyze)
  std::wstring text;
  std::wstring detail;
  ItemAction action = ItemAction::None;
  UiCommand command = UiCommand::Toggle;
  int index = 0;
  int sub = 0;  // a second index for the command: a server within a configuration
  bool checked = false;
  bool enabled = true;
  bool warn = false;    // something's wrong with it (a ProfileRow's refresh failed)
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

// The log's level groups, as its filter shows them.
constexpr int kLevelGroups = 4;
constexpr const wchar_t* kLevelNames[kLevelGroups] = {L"Ошибки", L"Предупр.", L"Инфо", L"Отладка"};

// A log line's group: 0 errors (and fatal, panic), 1 warnings, 2 info (and
// lines of no level), 3 debug and trace - by FormatLogLine's level column
// (main.cpp): "05:07:30  ERROR  outbound/...".
int LevelGroup(std::wstring_view line) {
  if (line.size() < 15 || line[8] != L' ' || line[9] != L' ') {
    return 2;
  }
  const std::wstring_view level = line.substr(10, 5);
  if (level.starts_with(L"ERROR") || level.starts_with(L"FATAL") || level.starts_with(L"PANIC")) {
    return 0;
  }
  if (level.starts_with(L"WARN")) {
    return 1;
  }
  if (level.starts_with(L"DEBUG") || level.starts_with(L"TRACE")) {
    return 3;
  }
  return 2;
}

// The log page's state: its lines (newest last) and the ones the level
// filter shows, how far those are scrolled and which are selected - from
// anchor to caret, inclusive, as positions among the shown. Paused, new lines
// wait in `held` and the view stays still.
struct LogView {
  std::deque<std::wstring> lines;
  std::vector<int> shown;  // indices into lines
  std::array<bool, kLevelGroups> levels{true, true, true, true};
  std::array<int, kLevelGroups> counts{};  // lines per group
  bool paused = false;
  std::vector<std::wstring> held;
  float scroll = 0;     // DIPs from the first shown line
  bool follow = true;   // stays at the bottom as lines come
  int anchor = -1;      // -1: no selection
  int caret = -1;

  int Count() const { return static_cast<int>(shown.size()); }
  const std::wstring& Line(int i) const { return lines[static_cast<std::size_t>(shown[static_cast<std::size_t>(i)])]; }
  bool HasSelection() const { return anchor >= 0 && caret >= 0 && caret < Count(); }
  bool Selected(int i) const { return HasSelection() && i >= std::min(anchor, caret) && i <= std::max(anchor, caret); }
  float Height() const { return static_cast<float>(shown.size()) * kLogLine; }

  void Rebuild() {
    shown.clear();
    counts.fill(0);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      const int group = LevelGroup(lines[i]);
      ++counts[static_cast<std::size_t>(group)];
      if (levels[static_cast<std::size_t>(group)]) {
        shown.push_back(static_cast<int>(i));
      }
    }
  }
};

// What the log page's layout shows of the log's state.
struct LogState {
  bool paused = false;
  std::size_t held = 0;
  std::array<int, kLevelGroups> counts{};
  std::array<bool, kLevelGroups> levels{true, true, true, true};
  std::wstring detail;  // the caret's line in full: the list cuts long ones
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

std::wstring StateDetailBase(const UiContent& c);

// The overview's lines under the state: which server and how fast, or what
// to do about it - and when the kill switch holds the internet closed, that.
std::wstring StateDetail(const UiContent& c) {
  std::wstring detail = StateDetailBase(c);
  if (c.killSwitchActive && c.display != Display::On && c.display != Display::Off) {
    detail += L"\nИнтернет закрыт kill switch'ем, пока подключение не восстановится.";
  } else if (c.display == Display::ServiceDown && c.killSwitch) {
    detail += L"\nЕсли включён kill switch, интернета нет, пока служба не запустится; снять вручную: "
              L"sovereign-core.exe --unblock от администратора.";
  }
  return detail;
}

std::wstring StateDetailBase(const UiContent& c) {
  switch (c.display) {
    case Display::ServiceDown:
      return L"sovereign-core не отвечает. Установи службу от администратора: sovereign-core.exe --install";
    case Display::Off:
      if (c.hasConfig) {
        return L"Кнопка внизу — подключиться.";
      }
      return c.profiles.empty() ? L"Сначала добавь подписку." : L"Включи конфигурацию: " + c.combineError;
    case Display::Starting: return L"Запускается ядро sing-box…";
    case Display::On:
      return std::format(L"↓ {}   ↑ {}   ·   соединений: {}", FormatRate(c.down), FormatRate(c.up), c.connections);
    case Display::Error: return c.error.empty() ? std::wstring(L"ядро не запустилось") : c.error;
  }
  return {};
}

bool AnyWaiting(const UiContent& c) {
  return std::any_of(c.profiles.begin(), c.profiles.end(), [](const UiProfile& p) { return p.waiting; });
}

// The configurations' line on the overview: which are on.
std::wstring SubscriptionLine(const UiContent& c) {
  if (c.profiles.empty()) {
    return L"нет — добавь ссылку или ключи";
  }
  std::vector<const UiProfile*> on;
  for (const UiProfile& p : c.profiles) {
    if (p.enabled) {
      on.push_back(&p);
    }
  }
  if (on.empty()) {
    return L"все выключены";
  }
  std::wstring line = on.size() == 1 ? on.front()->name : std::format(L"включено {} из {}", on.size(), c.profiles.size());
  if (AnyWaiting(c)) {
    return line + L" · новая версия ждёт";
  }
  if (std::any_of(on.begin(), on.end(), [](const UiProfile* p) { return p->failed; })) {
    return line + L" · ошибка обновления";
  }
  return on.size() == 1 && on.front()->subscription ? line + L" · " + on.front()->updated : line;
}
std::wstring AppsLine(const UiContent& c) {
  if (c.apps.empty()) {
    return c.appsInclude ? L"только список — он пуст" : L"всё через прокси";
  }
  return std::format(L"{} {}", c.appsInclude ? L"только" : L"всё, кроме", c.apps.size());
}

std::wstring UpdateStatus(const UiContent& c) {
  switch (c.update) {
    case UiUpdate::Idle: return L"Обновления проверяются сами, раз в 12 часов.";
    case UiUpdate::Checking: return L"Проверяю…";
    case UiUpdate::UpToDate: return L"Это последняя версия.";
    case UiUpdate::Available:
      return c.updateError.empty() ? L"Доступна версия " + c.updateVersion : L"Не удалось: " + c.updateError;
    case UiUpdate::Downloading: return L"Скачиваю версию " + c.updateVersion + L"…";
    case UiUpdate::Failed: return L"Не удалось проверить: " + c.updateError;
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
    title_ = MakeFormat(kDisplay, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    state_ = MakeFormat(kDisplay, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    delay_ = MakeFormat(kText, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    delay_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    heading_ = MakeFormat(kText, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    body_ = MakeFormat(kText, 14);
    caption_ = MakeFormat(kText, 12.5f);
    button_ = MakeFormat(kText, 14);
    button_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    chip_ = MakeFormat(kText, 12.5f);
    chip_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    center_ = MakeFormat(kText, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    center_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    wrap_ = MakeFormat(kText, 14, DWRITE_FONT_WEIGHT_NORMAL, true);
    captionWrap_ = MakeFormat(kText, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
    mono_ = MakeFormat(L"Consolas", 12.5f);
    const std::wstring glyphs = ui::GlyphFamily(dwrite_.get());
    glyph_ = MakeFormat(glyphs.c_str(), 16);
    glyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    glyphBig_ = MakeFormat(glyphs.c_str(), 24);
    glyphBig_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  }

  ID2D1Factory* D2d() const { return d2d_.get(); }

  // The layout of `page` in a client area of `width` x `height` DIPs, the
  // page scrolled by `scroll` (clamped here).
  Layout Build(const UiContent& c, UiPage page, const std::string& profile, float width, float height, float& scroll,
               const LogState& logState) const {
    Layout l;
    const float pageWidth = std::min(kMaxPage, std::max(280.0f, width - 2 * kPad));
    const float x0 = std::max(kPad, (width - pageWidth) / 2);
    const float x1 = x0 + pageWidth;
    l.viewTop = 0;
    l.viewBottom = height;
    if (page == UiPage::Overview) {
      Bar(l, c, width, height);  // fixed: before the page's items, which scroll
      l.viewBottom = height - kBar;
    }
    const std::size_t first = l.items.size();

    float y = kPad;
    switch (page) {
      case UiPage::Overview: y = Overview(l, c, x0, x1, y); break;
      case UiPage::Servers: y = Servers(l, c, x0, x1, y); break;
      case UiPage::Subscription: y = Subscription(l, c, x0, x1, y); break;
      case UiPage::Apps: y = Apps(l, c, x0, x1, y); break;
      case UiPage::Logs: y = Logs(l, c, logState, x0, x1, y, height); break;
      case UiPage::Settings: y = Settings(l, c, x0, x1, y); break;
      case UiPage::Profile: y = ProfilePage(l, c, profile, x0, x1, y); break;
    }
    l.contentHeight = y + kPad;

    scroll = std::clamp(scroll, 0.0f, std::max(0.0f, l.contentHeight - (l.viewBottom - l.viewTop)));
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

  // Draws a layout.
  void Draw(ID2D1RenderTarget* t, const Layout& l, const UiContent& c, const Interaction& in, const LogView& log) const {
    wil::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(t->CreateSolidColorBrush(Rgb(0, 0, 0), brush.put()))) {
      return;
    }
    Canvas k{t, brush.get()};
    const D2D1_SIZE_F size = t->GetSize();
    t->Clear(FromColorRef(ui::kWindowColor));

    bool clipped = false;
    for (std::size_t i = 0; i < l.items.size(); ++i) {
      const Item& it = l.items[i];
      if (it.scrolls && !clipped) {
        t->PushAxisAlignedClip({0, l.viewTop, size.width, l.viewBottom}, D2D1_ANTIALIAS_MODE_ALIASED);
        clipped = true;
      }
      if (it.scrolls && (it.rect.bottom < l.viewTop || it.rect.top > l.viewBottom)) {
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

  IWICImagingFactory* Wic() const { return wic_.get(); }

  // The program icons and the flags are the render target's: a new target
  // needs new ones.
  void ForgetIcons() const {
    icons_.clear();
    flags_.reset();
    flagsLoaded_ = false;
  }

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

  // A program's icon from its exe, at the target's DPI; null if the path is
  // unknown or the file has none. Loaded once per path (a miss too).
  ID2D1Bitmap* ProgramIcon(ID2D1RenderTarget* target, const std::wstring& path) const {
    if (path.empty() || !wic_) {
      return nullptr;
    }
    const auto [it, fresh] = icons_.try_emplace(path);
    if (!fresh) {
      return it->second.get();
    }
    FLOAT dpiX = 96;
    FLOAT dpiY = 96;
    target->GetDpi(&dpiX, &dpiY);
    const auto size = static_cast<UINT>(std::lround(kAppIcon * dpiX / 96.0f));
    HICON extracted = nullptr;
    if (FAILED(SHDefExtractIconW(path.c_str(), 0, 0, &extracted, nullptr, size)) || extracted == nullptr) {
      return nullptr;
    }
    const wil::unique_hicon icon(extracted);
    wil::com_ptr<IWICBitmap> source;
    wil::com_ptr<IWICFormatConverter> converter;
    if (SUCCEEDED(wic_->CreateBitmapFromHICON(icon.get(), source.put())) &&
        SUCCEEDED(wic_->CreateFormatConverter(converter.put())) &&
        SUCCEEDED(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0, WICBitmapPaletteTypeCustom))) {
      target->CreateBitmapFromWicBitmap(converter.get(), nullptr, it->second.put());
    }
    return it->second.get();
  }

  // The flags sprite (flags.h) from the exe's resources, as a bitmap of
  // `target` at 96 DPI - so a flag's source rectangle is its pixels. Null if
  // it can't be had; tried once per target.
  ID2D1Bitmap* Flags(ID2D1RenderTarget* target) const {
    if (flagsLoaded_ || !wic_) {
      return flags_.get();
    }
    flagsLoaded_ = true;
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_FLAGS), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL data = resource != nullptr ? LoadResource(module, resource) : nullptr;
    void* bytes = data != nullptr ? LockResource(data) : nullptr;
    if (bytes == nullptr) {
      return nullptr;
    }
    wil::com_ptr<IWICStream> stream;
    wil::com_ptr<IWICBitmapDecoder> decoder;
    wil::com_ptr<IWICBitmapFrameDecode> frame;
    wil::com_ptr<IWICFormatConverter> converter;
    if (SUCCEEDED(wic_->CreateStream(stream.put())) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(bytes), SizeofResource(module, resource))) &&
        SUCCEEDED(wic_->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) &&
        SUCCEEDED(decoder->GetFrame(0, frame.put())) && SUCCEEDED(wic_->CreateFormatConverter(converter.put())) &&
        SUCCEEDED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0, WICBitmapPaletteTypeCustom))) {
      const D2D1_BITMAP_PROPERTIES props =
          D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
      target->CreateBitmapFromWicBitmap(converter.get(), &props, flags_.put());
    }
    return flags_.get();
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

  // A page's top: back to the overview, the title, and a wrapped subtitle.
  float PageTitle(Layout& l, const wchar_t* title, const wchar_t* subtitle, float x0, float x1, float y,
                  UiPage backTo = UiPage::Overview) const {
    Item back = Make(Kind::IconButton, {x0 - 6, y, x0 + 30, y + 36}, L"Назад", kGlyphBack);
    back.action = ItemAction::Page;
    back.index = static_cast<int>(backTo);
    l.items.push_back(std::move(back));
    l.items.push_back(Make(Kind::Title, {x0 + 38, y, x1, y + 36}, title));
    y += 36 + 8;
    if (subtitle != nullptr) {
      const float h = TextHeight(subtitle, captionWrap_.get(), x1 - x0);
      l.items.push_back(Make(Kind::Caption, {x0, y, x1, y + h}, subtitle));
      y += h;
    }
    return y + 14;
  }

  float Paragraph(Layout& l, Kind kind, const std::wstring& text, float x0, float x1, float y) const {
    const float indent = kind == Kind::ErrorText ? 28.0f : 0.0f;
    const float h =
        std::max(20.0f, TextHeight(text, kind == Kind::Caption ? captionWrap_.get() : wrap_.get(), x1 - x0 - indent));
    l.items.push_back(Make(kind, {x0, y, x1, y + h}, text, kind == Kind::ErrorText ? kGlyphWarning : nullptr));
    return y + h;
  }

  // The one screen for every day: the state in a line or two, what needs a
  // click (an update, a subscription waiting), and tiles into the rest; the
  // switch and the server live in the bar under it (Bar).
  float Overview(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    // The state.
    const std::wstring detail = StateDetail(c);
    l.items.push_back(Make(Kind::Title, {x0, y, x1, y + 30}, StateTitle(c)));
    y += 32;
    const float detailHeight = TextHeight(detail, captionWrap_.get(), x1 - x0);
    Item state = Make(c.display == Display::Error ? Kind::ErrorText : Kind::Caption, {x0, y, x1, y + detailHeight},
                      detail, c.display == Display::Error ? kGlyphWarning : nullptr);
    if (c.display == Display::Error) {
      const float h = TextHeight(detail, wrap_.get(), x1 - x0 - 28);
      state.rect.bottom = y + h;
    }
    y = state.rect.bottom + kGap + 4;
    l.items.push_back(std::move(state));

    // What waits for a click: one line each.
    if (c.update == UiUpdate::Available || c.update == UiUpdate::Downloading) {
      Item update = CommandItem(Kind::Banner, {x0, y, x1, y + 40},
                                c.update == UiUpdate::Downloading ? L"Скачиваю версию " + c.updateVersion + L"…"
                                : c.updateError.empty()           ? L"Доступна версия " + c.updateVersion + L" — обновить"
                                                                  : L"Обновление не удалось — повторить",
                                kGlyphDownload, UiCommand::InstallUpdate);
      update.enabled = c.update == UiUpdate::Available;
      l.items.push_back(std::move(update));
      y += 40 + 8;
    }
    if (AnyWaiting(c)) {
      Item waiting = Make(Kind::Banner, {x0, y, x1, y + 40}, L"Новая версия подписки — выбрать", kGlyphSync);
      waiting.action = ItemAction::Page;
      waiting.index = static_cast<int>(UiPage::Subscription);
      l.items.push_back(std::move(waiting));
      y += 40 + 8;
    }

    // Nothing to connect with yet: the way to fix that comes first.
    if (c.profiles.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 108}));
      l.items.push_back(Make(Kind::Text, {x0 + 16, y + 12, x1 - 16, y + 36}, L"Нет конфигурации"));
      l.items.push_back(
          Make(Kind::Muted, {x0 + 16, y + 36, x1 - 16, y + 56}, L"Скопируй ссылку, ключи, конфиг или QR-код."));
      l.items.push_back(CommandItem(Kind::AccentButton, {x0 + 16, y + 64, x1 - 16, y + 64 + kButton},
                                    L"Вставить из буфера", kGlyphPaste, UiCommand::PasteSubscription));
      y += 108 + kGap;
    }

    // Into the rest: two tiles a row.
    const float mid = (x0 + x1) / 2;
    const auto tile = [&](int n, const wchar_t* glyph, const wchar_t* title, std::wstring line, UiPage page, bool dot) {
      const float left = n % 2 == 0 ? x0 : mid + 5;
      const float right = n % 2 == 0 ? mid - 5 : x1;
      const int row = n / 2;
      const float top = y + static_cast<float>(row) * (kTile + 10);
      Item t = Make(Kind::Tile, {left, top, right, top + kTile}, title, glyph);
      t.detail = std::move(line);
      t.action = ItemAction::Page;
      t.index = static_cast<int>(page);
      t.checked = dot;
      l.items.push_back(std::move(t));
    };
    tile(0, kGlyphSync, L"Конфигурации", SubscriptionLine(c), UiPage::Subscription, AnyWaiting(c));
    tile(1, kGlyphProgram, L"Приложения", AppsLine(c), UiPage::Apps, false);
    tile(2, kGlyphLog, L"Журнал", c.display == Display::Error ? L"есть ошибка" : L"ядро и трей", UiPage::Logs,
         c.display == Display::Error);
    tile(3, kGlyphSettings, L"Настройки", L"Sovereign " + c.version.substr(0, c.version.find(L' ')), UiPage::Settings,
         c.update == UiUpdate::Available);
    y += 2 * kTile + 10 + kGap;

    // The kill switch: on the overview, it's what decides what a drop does.
    return KillSwitchCard(l, c, x0, x1, y);
  }

  float KillSwitchCard(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 64}));
    Item kill = CommandItem(Kind::Switch, {x0 + 4, y + 4, x1 - 4, y + 60}, L"Kill switch", nullptr,
                            UiCommand::ToggleKillSwitch);
    kill.checked = c.killSwitch;
    if (!c.killSwitch) {
      kill.detail = L"при обрыве трафик пойдёт напрямую";
    } else if (c.killSwitchActive && c.display != Display::On && c.display != Display::Off) {
      kill.detail = L"интернет закрыт до восстановления";
    } else {
      kill.detail = L"при обрыве трафик не пойдёт мимо прокси";
    }
    l.items.push_back(std::move(kill));
    y += 64;
    if (!c.killSwitchError.empty()) {
      y = Paragraph(l, Kind::ErrorText, L"Kill switch: " + c.killSwitchError, x0, x1, y + 8);
    }
    return y;
  }

  // The overview's bottom: the switch, and under it the server in use - a
  // click opens the servers.
  void Bar(Layout& l, const UiContent& c, float width, float height) const {
    const float top = height - kBar;
    Item bar = Make(Kind::Bar, {0, top, width, height});
    bar.scrolls = false;
    l.items.push_back(std::move(bar));
    const float cx = width / 2;
    Item power = CommandItem(Kind::Power, {cx - kPower / 2, top + 12, cx + kPower / 2, top + 12 + kPower}, {},
                             kGlyphPower, UiCommand::Toggle);
    power.checked = c.on;
    power.scrolls = false;
    l.items.push_back(std::move(power));
    std::wstring server = CurrentProtocol(c);
    if (!server.empty() && static_cast<std::size_t>(c.protocol) < c.delays.size()) {
      if (const std::wstring delay = DelayLabel(c.delays[static_cast<std::size_t>(c.protocol)]); !delay.empty()) {
        server += L" · " + delay;
      }
    }
    const float linkTop = top + 12 + kPower + 4;
    Item link = Make(Kind::ServerLink, {cx - 150, linkTop, cx + 150, linkTop + 30},
                     server.empty() ? std::wstring(L"выбор сервера") : server, kGlyphChevron);
    link.action = ItemAction::Page;
    link.index = static_cast<int>(UiPage::Servers);
    link.enabled = !c.protocols.empty();
    link.scrolls = false;
    const float exitTop = link.rect.bottom;
    l.items.push_back(std::move(link));

    // The exit: the flag, and the address or - hidden with a click - the country.
    if (c.exitIp.empty() && !c.exitPending) {
      return;
    }
    std::wstring text;
    if (c.exitIp.empty()) {
      text = L"определяю выходной IP…";
    } else if (c.hideExitIp) {
      text = c.exitCountryName.empty() ? std::wstring(L"IP скрыт") : c.exitCountryName;
    } else {
      text = c.exitCountryName.empty() ? c.exitIp : c.exitIp + L" · " + c.exitCountryName;
    }
    Item exit = CommandItem(Kind::ExitLine, {cx - 160, exitTop, cx + 160, exitTop + 24}, std::move(text), nullptr,
                            UiCommand::ToggleExitIp);
    exit.detail = c.exitCountry;
    exit.enabled = !c.exitIp.empty();
    exit.scrolls = false;
    l.items.push_back(std::move(exit));
  }

  // The servers of the subscription's selector, with their latency.
  float Servers(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    const float titleTop = y;
    std::wstring note = L"Задержка — запрос через каждый сервер; меньше — лучше.";
    if (c.delaysTesting) {
      note = L"Проверяю задержку…";
    } else if (!c.canTestDelays && !c.protocols.empty()) {
      note = L"Задержку можно проверить, когда подключение включено.";
    } else if (!c.delayError.empty()) {
      note = L"Не удалось проверить: " + c.delayError;
    }
    y = PageTitle(l, L"Серверы", note.c_str(), x0, x1, y);
    Item test = CommandItem(Kind::IconButton, {x1 - 36, titleTop, x1, titleTop + 36}, L"Проверить задержку",
                            kGlyphStopwatch, UiCommand::TestDelays);
    test.enabled = c.canTestDelays && !c.delaysTesting;
    l.items.push_back(std::move(test));
    if (c.protocols.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 56}));
      l.items.push_back(Make(Kind::Muted, {x0 + 16, y, x1 - 16, y + 56},
                             c.hasConfig ? L"В конфиге нет выбора сервера." : L"Ни одна конфигурация не включена."));
      return y + 56;
    }
    const float h = static_cast<float>(c.protocols.size()) * kRow;
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
    for (std::size_t i = 0; i < c.protocols.size(); ++i) {
      const float top = y + static_cast<float>(i) * kRow;
      if (i > 0) {
        l.items.push_back(Make(Kind::Divider, {x0 + 16, top, x1 - 16, top + 1}));
      }
      Item choice = CommandItem(Kind::Choice, {x0 + 4, top + 3, x1 - 4, top + kRow - 3}, c.protocols[i], nullptr,
                                UiCommand::SetProtocol, static_cast<int>(i));
      choice.checked = static_cast<int>(i) == c.protocol;
      l.items.push_back(std::move(choice));
    }
    return y + h;
  }

  float Subscription(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = PageTitle(l, L"Конфигурации",
                  L"Подписки, ключи и свои конфиги. Включённые работают вместе: их серверы — в общем списке "
                  L"«Серверы», маршруты и DNS — от первой включённой. Нажми на конфигурацию, чтобы "
                  L"переименовать её, настроить обновление или выключить отдельные серверы.",
                  x0, x1, y);

    // Every configuration: the switch turns it on and off, a click opens it.
    if (c.profiles.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 56}));
      l.items.push_back(Make(Kind::Muted, {x0 + 16, y, x1 - 16, y + 56},
                             L"Пока ни одной — добавь ссылку, ключи, конфиг или QR-код."));
      y += 56;
    } else {
      const float h = static_cast<float>(c.profiles.size()) * kProfileRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t i = 0; i < c.profiles.size(); ++i) {
        const UiProfile& p = c.profiles[i];
        const float top = y + static_cast<float>(i) * kProfileRow;
        if (i > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 16, top, x1 - 16, top + 1}));
        }
        Item row = Make(Kind::ProfileRow, {x0 + 4, top + 3, x1 - 72, top + kProfileRow - 3}, p.name, kGlyphChevron);
        row.action = ItemAction::OpenProfile;
        row.index = static_cast<int>(i);
        row.detail = p.detail;
        row.checked = p.enabled;
        row.warn = p.failed;
        l.items.push_back(std::move(row));
        Item toggle = CommandItem(Kind::Toggle, {x1 - 68, top + 3, x1 - 4, top + kProfileRow - 3},
                                  p.enabled ? L"Выключить" : L"Включить", nullptr, UiCommand::ToggleProfile,
                                  static_cast<int>(i));
        toggle.checked = p.enabled;
        l.items.push_back(std::move(toggle));
      }
      y += h;
    }
    y += kGap;
    if (!c.profiles.empty() && !c.hasConfig && !c.combineError.empty()) {
      y = Paragraph(l, Kind::ErrorText, L"Нечего запускать: " + c.combineError, x0, x1, y) + kGap;
    }
    for (const std::wstring& note : c.combineNotes) {
      y = Paragraph(l, Kind::Caption, L"Не вошло: " + note, x0, x1, y) + 6;
    }

    // Adding one: the clipboard (a link, keys, a config, a picture with a QR
    // code, files), a file, QR codes on the screen.
    const float mid = (x0 + x1) / 2;
    l.items.push_back(CommandItem(Kind::AccentButton, {x0, y, x1, y + kButton}, L"Добавить из буфера", kGlyphAdd,
                                  UiCommand::PasteSubscription));
    y += kButton + 8;
    l.items.push_back(CommandItem(Kind::Button, {x0, y, mid - 4, y + kButton}, L"Файл…", kGlyphFile,
                                  UiCommand::ImportFile));
    l.items.push_back(CommandItem(Kind::Button, {mid + 4, y, x1, y + kButton}, L"QR с экрана", kGlyphQrCode,
                                  UiCommand::ScanScreen));
    return y + kButton;
  }

  // One configuration: its name, on/off, its subscription's refreshing, a
  // waiting version, and its servers - each can be switched off.
  float ProfilePage(Layout& l, const UiContent& c, const std::string& id, float x0, float x1, float y) const {
    const auto found = std::find_if(c.profiles.begin(), c.profiles.end(), [&](const UiProfile& p) { return p.id == id; });
    if (found == c.profiles.end()) {
      y = PageTitle(l, L"Конфигурация", nullptr, x0, x1, y, UiPage::Subscription);
      l.items.push_back(Make(Kind::Muted, {x0, y, x1, y + 24}, L"Её больше нет."));
      return y + 24;
    }
    const UiProfile& p = *found;
    const int index = static_cast<int>(found - c.profiles.begin());
    y = PageTitle(l, p.name.c_str(), nullptr, x0, x1, y, UiPage::Subscription);
    for (auto it = l.items.rbegin(); it != l.items.rend(); ++it) {
      if (it->kind == Kind::Title) {  // a click on the name renames it
        it->action = ItemAction::EditName;
        it->index = index;
        break;
      }
    }
    const float mid = (x0 + x1) / 2;

    const float cardHeight = p.subscription ? 128.0f : 64.0f;
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + cardHeight}));
    Item on = CommandItem(Kind::Switch, {x0 + 4, y + 4, x1 - 4, y + 60}, L"Включена", nullptr, UiCommand::ToggleProfile,
                          index);
    on.detail = p.enabled ? L"её серверы — в общем списке" : L"не участвует в подключении";
    on.checked = p.enabled;
    l.items.push_back(std::move(on));
    if (p.subscription) {
      l.items.push_back(Make(Kind::Divider, {x0 + 16, y + 64, x1 - 16, y + 65}));
      Item autoUpdate = CommandItem(Kind::Switch, {x0 + 4, y + 68, x1 - 4, y + 124}, L"Автообновление", nullptr,
                                    UiCommand::ToggleAutoUpdate, index);
      autoUpdate.detail = p.autoUpdate ? p.period : L"только кнопкой «Обновить»";
      autoUpdate.checked = p.autoUpdate;
      l.items.push_back(std::move(autoUpdate));
    }
    y += cardHeight + 8;    Item rename = Make(Kind::Button, {x0, y, mid - 4, y + kButton}, L"Переименовать", kGlyphRename);
    rename.action = ItemAction::EditName;
    rename.index = index;
    l.items.push_back(std::move(rename));
    if (p.subscription) {
      l.items.push_back(CommandItem(Kind::Button, {mid + 4, y, x1, y + kButton}, L"Период…", kGlyphClock,
                                    UiCommand::ChooseRefreshPeriod, index));
    }
    y += kButton + kGap;

    if (p.subscription) {
      const std::vector<std::pair<const wchar_t*, std::wstring>> fields = {
          {L"Обновлена", p.failed ? std::wstring(L"не удалась") : p.updated},
          {L"Сервер", p.host.empty() ? std::wstring(L"—") : p.host},
          {L"Конфиг", p.edited ? L"изменён тобой" : L"как в подписке"},
      };
      const float h = static_cast<float>(fields.size()) * kRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t i = 0; i < fields.size(); ++i) {
        const float top = y + static_cast<float>(i) * kRow;
        if (i > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 16, top, x1 - 16, top + 1}));
        }
        Item field = Make(Kind::Field, {x0 + 16, top, x1 - 16, top + kRow}, fields[i].first);
        field.detail = fields[i].second;
        l.items.push_back(std::move(field));
      }
      y += h + kGap;
      if (!p.error.empty()) {
        y = Paragraph(l, Kind::ErrorText, p.error, x0, x1, y) + kGap;
      }
      if (p.waiting) {
        y = Waiting(l, p, index, x0, x1, y) + kGap;
      }
      if (!p.mergeNotes.empty()) {
        std::wstring where;
        for (std::size_t i = 0; i < p.mergeNotes.size() && i < 5; ++i) {
          where += (i > 0 ? L", " : L"") + p.mergeNotes[i];
        }
        if (p.mergeNotes.size() > 5) {
          where += std::format(L" и ещё {}", p.mergeNotes.size() - 5);
        }
        y = Paragraph(l, Kind::Wrap,
                      L"Правки перенесены. Там, где вы с подпиской изменили одно и то же, осталось твоё — проверь: " + where,
                      x0, x1, y) + kGap;
      }
      l.items.push_back(CommandItem(Kind::Button, {x0, y, mid - 4, y + kButton}, L"Обновить", kGlyphRefresh,
                                    UiCommand::RefreshProfile, index));
      l.items.push_back(CommandItem(Kind::Button, {mid + 4, y, x1, y + kButton}, L"Копировать ссылку", kGlyphCopy,
                                    UiCommand::CopyProfileLink, index));
      y += kButton + 8;
      if (p.edited || p.waiting) {
        l.items.push_back(CommandItem(Kind::Button, {x0, y, x1, y + kButton}, L"Как в подписке", kGlyphUndo,
                                      UiCommand::RevertConfig, index));
        y += kButton + 8;
      }
      y += kGap - 8;
    }

    // Its servers: a switch each.
    const auto off = std::count_if(p.servers.begin(), p.servers.end(), [](const UiServer& s) { return !s.enabled; });
    l.items.push_back(Make(Kind::Heading, {x0, y, x1, y + 28},
                           off > 0 ? std::format(L"Серверы: {}, выключено {}", p.servers.size(), off)
                                   : std::format(L"Серверы: {}", p.servers.size())));
    y += 28 + 8;
    if (p.servers.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 56}));
      l.items.push_back(Make(Kind::Muted, {x0 + 16, y, x1 - 16, y + 56}, L"В конфиге нет серверов."));
      y += 56;
    } else {
      const float h = static_cast<float>(p.servers.size()) * kServerRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t i = 0; i < p.servers.size(); ++i) {
        const float top = y + static_cast<float>(i) * kServerRow;
        if (i > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 16, top, x1 - 16, top + 1}));
        }
        Item server = CommandItem(Kind::Switch, {x0 + 4, top + 2, x1 - 4, top + kServerRow - 2}, p.servers[i].name,
                                  nullptr, UiCommand::ToggleServer, index);
        server.sub = static_cast<int>(i);
        server.detail = p.servers[i].label;
        server.checked = p.servers[i].enabled;
        l.items.push_back(std::move(server));
      }
      y += h;
    }
    y += kGap;
    l.items.push_back(CommandItem(Kind::DangerButton, {x0, y, x1, y + kButton}, L"Удалить конфигурацию", kGlyphDelete,
                                  UiCommand::RemoveProfile, index));
    return y + kButton;
  }

  // A newer subscription met the user's edits: the three ways on.
  float Waiting(Layout& l, const UiProfile& p, int index, float x0, float x1, float y) const {
    const float left = x0 + 16;
    const float right = x1 - 16;
    const wchar_t* explain = L"В конфиге есть твои правки, поэтому он не заменён сам — пока работает прежний. "
                             L"Перенести: твои правки лягут поверх новой версии. Что бы ты ни выбрал, "
                             L"прежний конфиг сохранится в папке history.";
    const float textHeight = TextHeight(explain, wrap_.get(), right - left);
    const bool inRow = right - left >= 3 * 150 + 2 * 12;
    const float buttons = inRow ? kButton : 3 * kButton + 2 * 8;
    const std::wstring& error = p.choiceError;
    const float errorHeight = error.empty() ? 0 : TextHeight(error, wrap_.get(), right - left - 28) + 12;
    const float h = 16 + 28 + 6 + textHeight + 16 + errorHeight + buttons + 20;
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
    float top = y + 16;
    l.items.push_back(Make(Kind::Heading, {left, top, right, top + 28}, L"Пришла новая версия подписки"));
    top += 28 + 6;
    l.items.push_back(Make(Kind::Wrap, {left, top, right, top + textHeight}, explain));
    top += textHeight + 16;
    if (!error.empty()) {
      top = Paragraph(l, Kind::ErrorText, error, left, right, top) + 12;
    }
    const std::array<std::tuple<Kind, const wchar_t*, const wchar_t*, UiCommand>, 3> choices = {{
        {Kind::AccentButton, L"Перенести правки", kGlyphSync, UiCommand::CarryOverEdits},
        {Kind::Button, L"Взять новую", kGlyphDownload, UiCommand::TakeSubscription},
        {Kind::Button, L"Оставить мою", kGlyphCheck, UiCommand::KeepConfig},
    }};
    const float width = inRow ? (right - left - 2 * 12) / 3 : right - left;
    for (std::size_t i = 0; i < choices.size(); ++i) {
      const auto& [kind, label, glyph, command] = choices[i];
      const float bx = inRow ? left + static_cast<float>(i) * (width + 12) : left;
      const float by = inRow ? top : top + static_cast<float>(i) * (kButton + 8);
      l.items.push_back(CommandItem(kind, {bx, by, bx + width, by + kButton}, label, glyph, command, index));
    }
    return y + h;
  }

  float Apps(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = PageTitle(l, L"Приложения", L"Какие программы идут через прокси. Имена — как у exe-файла, без пути.", x0, x1, y);
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
                  c.appsInclude ? L"Через прокси идут только программы из списка, остальной трафик — напрямую."
                                : L"Весь трафик идёт через прокси, кроме программ из списка — они ходят напрямую.",
                  x0, x1, y) +
        kGap;

    if (c.apps.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 64}));
      l.items.push_back(Make(Kind::Muted, {x0 + 16, y, x1 - 16, y + 64},
                             c.appsInclude ? L"Список пуст — через прокси не идёт ничего." : L"Список пуст — всё идёт через прокси."));
      y += 64;
    } else {
      const float h = static_cast<float>(c.apps.size()) * kRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t i = 0; i < c.apps.size(); ++i) {
        const float top = y + static_cast<float>(i) * kRow;
        if (i > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 16, top, x1 - 16, top + 1}));
        }
        Item app = Make(Kind::AppRow, {x0 + 8, top, x1 - 56, top + kRow}, c.apps[i], kGlyphProgram);
        app.detail = i < c.appPaths.size() ? c.appPaths[i] : std::wstring();  // the icon's exe
        l.items.push_back(std::move(app));
        const float bt = top + (kRow - kButton) / 2;
        Item remove = CommandItem(Kind::IconButton, {x1 - 12 - kButton, bt, x1 - 12, bt + kButton}, L"Убрать",
                                  kGlyphRemove, UiCommand::RemoveApp, static_cast<int>(i));
        l.items.push_back(std::move(remove));
      }
      y += h;
    }
    y += kGap;
    const float half = (x0 + x1) / 2;
    l.items.push_back(CommandItem(Kind::Button, {x0, y, half - 4, y + kButton}, L"Из запущенных", kGlyphAdd,
                                  UiCommand::AddRunning));
    l.items.push_back(
        CommandItem(Kind::Button, {half + 4, y, x1, y + kButton}, L"Файл exe…", kGlyphFile, UiCommand::AddExe));
    return y + kButton;
  }

  // The log: in the title's row the level the core writes, pause and save;
  // the level filter; the lines, filling the window; the selected line in
  // full under them. Right-click for copy, select all, save, clear.
  float Logs(Layout& l, const UiContent& c, const LogState& s, float x0, float x1, float y, float height) const {
    const float titleTop = y;
    y = PageTitle(l, L"Журнал", nullptr, x0, x1, y);
    float right = x1;
    Item save = Make(Kind::IconButton, {right - 36, titleTop, right, titleTop + 36}, L"Сохранить в файл", kGlyphSave);
    save.action = ItemAction::SaveLogs;
    l.items.push_back(std::move(save));
    right -= 40;
    const bool waiting = s.paused && s.held > 0;
    const float pauseWidth = waiting ? 84.0f : 36.0f;
    Item pause = Make(waiting ? Kind::Button : Kind::IconButton, {right - pauseWidth, titleTop, right, titleTop + 36},
                      waiting ? std::format(L"+{}", s.held) : std::wstring(s.paused ? L"Продолжить" : L"Пауза"),
                      s.paused ? kGlyphPlay : kGlyphPause);
    pause.action = ItemAction::PauseLogs;
    pause.checked = s.paused;
    l.items.push_back(std::move(pause));
    right -= pauseWidth + 4;
    l.items.push_back(CommandItem(Kind::Button, {right - 124, titleTop + 1, right, titleTop + 1 + kButton},
                                  c.logLevel.empty() ? std::wstring(L"запись: авто") : L"запись: " + c.logLevel,
                                  kGlyphFilter, UiCommand::ChooseLogLevel));

    // Which levels are shown, with how many lines each.
    const float chip = (x1 - x0 - 3 * 6) / kLevelGroups;
    for (int i = 0; i < kLevelGroups; ++i) {
      const float left = x0 + static_cast<float>(i) * (chip + 6);
      Item level = Make(Kind::Chip, {left, y, left + chip, y + 30},
                        std::format(L"{} {}", kLevelNames[i], s.counts[static_cast<std::size_t>(i)]));
      level.action = ItemAction::ToggleLevel;
      level.index = i;
      level.checked = s.levels[static_cast<std::size_t>(i)];
      l.items.push_back(std::move(level));
    }
    y += 30 + 10;

    // Fills the window: the lines scroll inside, the page doesn't.
    const float detailHeight =
        s.detail.empty() ? 0.0f : std::min(84.0f, TextHeight(s.detail, captionWrap_.get(), x1 - x0 - 28)) + 20;
    const float bottom = std::max(y + 120, height - kPad - (detailHeight > 0 ? detailHeight + 8 : 0.0f));
    l.items.push_back(Make(Kind::Card, {x0, y, x1, bottom}));
    l.items.push_back(Make(Kind::LogBox, {x0, y, x1, bottom}));
    l.logBox = D2D1_RECT_F{x0 + 1, y + 8, x1 - 1, bottom - 8};
    if (detailHeight > 0) {
      const float top = bottom + 8;
      l.items.push_back(Make(Kind::Card, {x0, top, x1, top + detailHeight}));
      l.items.push_back(Make(Kind::Caption, {x0 + 14, top + 10, x1 - 14, top + detailHeight - 10}, s.detail));
      return top + detailHeight - kPad + 1;
    }
    return bottom - kPad + 1;  // exactly the view: nothing to scroll
  }

  float Settings(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = PageTitle(l, L"Настройки", nullptr, x0, x1, y);

    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 68}));
    Item autostart = CommandItem(Kind::Switch, {x0 + 4, y + 4, x1 - 4, y + 64}, L"Запуск при входе в Windows", nullptr,
                                 UiCommand::ToggleAutostart);
    autostart.detail = L"В трее, без окна.";
    autostart.checked = c.autostart;
    l.items.push_back(std::move(autostart));
    y += 68 + kGap;

    // The kill switch and what it lets through.
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 128}));
    Item kill = CommandItem(Kind::Switch, {x0 + 4, y + 4, x1 - 4, y + 60}, L"Kill switch", nullptr,
                            UiCommand::ToggleKillSwitch);
    kill.detail = L"Трафик не пойдёт мимо прокси.";
    kill.checked = c.killSwitch;
    l.items.push_back(std::move(kill));
    l.items.push_back(Make(Kind::Divider, {x0 + 16, y + 64, x1 - 16, y + 65}));
    Item lan = CommandItem(Kind::Switch, {x0 + 4, y + 68, x1 - 4, y + 124}, L"Локальная сеть", nullptr,
                           UiCommand::ToggleKillSwitchLan);
    lan.detail = L"Принтер, NAS — доступны и при обрыве.";
    lan.checked = c.killSwitchLan;
    lan.enabled = c.killSwitch;
    l.items.push_back(std::move(lan));
    y += 128 + kGap;
    if (!c.killSwitchError.empty()) {
      y = Paragraph(l, Kind::ErrorText, L"Kill switch: " + c.killSwitchError, x0, x1, y) + kGap;
    }

    // Version and updates: the button under the text, the window is narrow.
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 110}));
    l.items.push_back(Make(Kind::Text, {x0 + 16, y + 10, x1 - 16, y + 34}, L"Sovereign " + c.version));
    l.items.push_back(Make(Kind::Muted, {x0 + 16, y + 34, x1 - 16, y + 56}, UpdateStatus(c)));
    const D2D1_RECT_F button{x0 + 16, y + 64, x1 - 16, y + 64 + kButton};
    if (c.update == UiUpdate::Available) {
      l.items.push_back(CommandItem(Kind::AccentButton, button, L"Обновить до " + c.updateVersion, kGlyphDownload,
                                    UiCommand::InstallUpdate));
    } else {
      Item check = CommandItem(Kind::Button, button,
                               c.update == UiUpdate::Downloading ? L"Скачиваю…" : L"Проверить обновления",
                               kGlyphRefresh, UiCommand::CheckUpdate);
      check.enabled = c.update != UiUpdate::Checking && c.update != UiUpdate::Downloading;
      l.items.push_back(std::move(check));
    }
    y += 110 + kGap;

    const float mid = (x0 + x1) / 2;
    l.items.push_back(CommandItem(Kind::Button, {x0, y, mid - 4, y + kButton}, L"Папка данных", kGlyphFolder,
                                  UiCommand::OpenFolder));
    l.items.push_back(CommandItem(Kind::DangerButton, {mid + 4, y, x1, y + kButton}, L"Выйти", kGlyphExit,
                                  UiCommand::Exit));
    y += kButton + 10;
    return Paragraph(l, Kind::Caption,
                     L"Папка — %LOCALAPPDATA%\\Sovereign: настройки, конфиг, его история. "
                     L"«Выйти» закрывает трей; подключение остаётся — им управляет служба, "
                     L"и следующий запуск продолжит с того же места.",
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
      case Kind::Caption: k.Text(it.text, captionWrap_.get(), r, secondary); break;
      case Kind::ErrorText: {
        const D2D1_COLOR_F danger = FromColorRef(ui::kDanger);
        k.Text(it.glyph, glyph_.get(), {r.left, r.top, r.left + 20, r.top + 20}, danger);
        k.Text(it.text, wrap_.get(), {r.left + 28, r.top, r.right, r.bottom}, danger);
        break;
      }
      case Kind::Field:
        k.Text(it.text, body_.get(), {r.left, r.top, r.left + 110, r.bottom}, secondary);
        k.Text(it.detail, body_.get(), {r.left + 110, r.top, r.right, r.bottom}, primary);
        break;
      case Kind::Power: DrawPower(k, it, c, hovered, pressed); break;
      case Kind::Tile: {
        k.Round(r, kRadius, Rgb(255, 255, 255, 0.045f + hoverAlpha * 0.6f));
        k.Outline(r, kRadius, Rgb(255, 255, 255, 0.06f));
        k.Text(it.glyph, glyph_.get(), {r.left + 12, r.top + 10, r.left + 36, r.top + 34}, accent);
        k.Text(it.text, body_.get(), {r.left + 14, r.top + 38, r.right - 10, r.top + 56}, primary);
        k.Text(it.detail, caption_.get(), {r.left + 14, r.top + 55, r.right - 10, r.bottom - 6}, secondary);
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse({r.right - 14, r.top + 14}, 4, 4), k.Color(accent));
        }
        break;
      }
      case Kind::Bar:
        k.Fill(r, FromColorRef(ui::kPanelColor));
        k.Line({r.left, r.top + 0.5f}, {r.right, r.top + 0.5f}, Rgb(255, 255, 255, 0.07f));
        break;
      case Kind::ServerLink: {
        if (hoverAlpha > 0 && it.enabled) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha * 0.6f));
        }
        // The name centered, the chevron right after it.
        const D2D1_COLOR_F ink = it.enabled ? primary : secondary;
        float textWidth = r.right - r.left - 48;
        wil::com_ptr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwrite_->CreateTextLayout(it.text.data(), static_cast<UINT32>(it.text.size()), center_.get(),
                                                textWidth, r.bottom - r.top, layout.put()))) {
          DWRITE_TEXT_METRICS m{};
          if (SUCCEEDED(layout->GetMetrics(&m))) {
            textWidth = std::min(textWidth, m.width);
          }
        }
        const float left = (r.left + r.right - textWidth - (it.enabled ? 22.0f : 0.0f)) / 2;
        k.Text(it.text, center_.get(), {left - 2, r.top, left + textWidth + 2, r.bottom}, ink);
        if (it.enabled) {
          k.Text(it.glyph, glyph_.get(), {left + textWidth + 4, r.top, left + textWidth + 22, r.bottom}, secondary);
        }
        break;
      }
      case Kind::ExitLine: {
        // The flag and the text, centered together.
        float textWidth = r.right - r.left - kFlagW - 8;
        wil::com_ptr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwrite_->CreateTextLayout(it.text.data(), static_cast<UINT32>(it.text.size()), caption_.get(),
                                                textWidth, r.bottom - r.top, layout.put()))) {
          DWRITE_TEXT_METRICS m{};
          if (SUCCEEDED(layout->GetMetrics(&m))) {
            textWidth = std::min(textWidth, m.width);
          }
        }
        const std::optional<std::size_t> flag = FlagIndex(std::wstring_view(it.detail).size() == 2
                                                              ? std::string{static_cast<char>(it.detail[0]),
                                                                            static_cast<char>(it.detail[1])}
                                                              : std::string());
        ID2D1Bitmap* sprite = flag ? Flags(k.t) : nullptr;
        const float flagWidth = sprite != nullptr ? kFlagW + 8 : 0.0f;
        const float left = (r.left + r.right - textWidth - flagWidth) / 2;
        if (hovered && it.enabled) {
          k.Round({left - 8, r.top + 1, left + flagWidth + textWidth + 8, r.bottom - 1}, 6,
                  Rgb(255, 255, 255, pressed ? 0.04f : 0.07f));
        }
        if (sprite != nullptr) {
          const auto n = static_cast<float>(*flag);
          const float column = std::fmod(n, static_cast<float>(kFlagColumns));
          const float row = std::floor(n / static_cast<float>(kFlagColumns));
          const D2D1_RECT_F source{column * kFlagWidth, row * kFlagHeight, (column + 1) * kFlagWidth,
                                   (row + 1) * kFlagHeight};
          const float cy = (r.top + r.bottom) / 2;
          const D2D1_RECT_F place{left, cy - kFlagH / 2, left + kFlagW, cy + kFlagH / 2};
          k.t->DrawBitmap(sprite, place, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
          k.Outline(place, 1.5f, Rgb(255, 255, 255, 0.12f));
        }
        k.Text(it.text, caption_.get(), {left + flagWidth, r.top, left + flagWidth + textWidth + 2, r.bottom},
               it.enabled ? secondary : FromColorRef(ui::kSecondaryText, 0.7f));
        break;
      }
      case Kind::Banner: {
        k.Round(r, kRadius, FromColorRef(ui::kAccent, pressed ? 0.10f : (hovered ? 0.18f : 0.13f)));
        const D2D1_COLOR_F ink = it.enabled ? accent : secondary;
        k.Text(it.glyph, glyph_.get(), {r.left + 10, r.top, r.left + 38, r.bottom}, ink);
        k.Text(it.text, body_.get(), {r.left + 44, r.top, r.right - 36, r.bottom}, ink);
        if (it.enabled) {
          k.Text(kGlyphChevron, glyph_.get(), {r.right - 34, r.top, r.right - 8, r.bottom}, ink);
        }
        break;
      }
      case Kind::Button:
      case Kind::AccentButton:
      case Kind::DangerButton: DrawButton(k, it, hovered, pressed); break;
      case Kind::IconButton:
        if (hoverAlpha > 0 && it.enabled) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        k.Text(it.glyph, glyph_.get(), r,
               !it.enabled ? FromColorRef(ui::kSecondaryText, 0.4f) : (hovered ? primary : secondary));
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse({r.right - 9, r.top + 9}, 4, 4), k.Color(accent));
        }
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
        const D2D1_POINT_2F dot{r.left + 22, (r.top + r.bottom) / 2};
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse(dot, 9, 9), k.Color(accent));
          k.t->FillEllipse(D2D1::Ellipse(dot, 4, 4), k.Color(Rgb(12, 20, 36)));
        } else {
          k.t->DrawEllipse(D2D1::Ellipse(dot, 8.5f, 8.5f), k.Color(secondary), 1.2f);
        }
        k.Text(it.text, body_.get(), {r.left + 42, r.top, r.right - 100, r.bottom}, primary);
        if (it.index >= 0 && static_cast<std::size_t>(it.index) < c.delays.size()) {
          const UiDelay& delay = c.delays[static_cast<std::size_t>(it.index)];
          k.Text(DelayLabel(delay), delay_.get(), {r.right - 100, r.top, r.right - 12, r.bottom},
                 FromColorRef(ui::DelayColor(delay)));
        }
        break;
      }
      case Kind::ProfileRow: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha * 0.6f));
        }
        const float mid = (r.top + r.bottom) / 2;
        k.Text(it.text, body_.get(), {r.left + 14, r.top + 2, r.right - 28, mid + 1}, it.checked ? primary : secondary);
        k.Text(it.detail, caption_.get(), {r.left + 14, mid + 1, r.right - 28, r.bottom - 2},
               it.warn ? FromColorRef(ui::kDanger) : secondary);
        k.Text(it.glyph, glyph_.get(), {r.right - 26, r.top, r.right - 4, r.bottom}, secondary);
        break;
      }
      case Kind::Toggle: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha * 0.6f));
        }
        const float cx = (r.left + r.right) / 2;
        const float cy = (r.top + r.bottom) / 2;
        DrawSwitch(k, {cx - 20, cy - 10, cx + 20, cy + 10}, it.checked);
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
      case Kind::Chip:
        if (it.checked) {
          k.Round(r, 15, FromColorRef(ui::kAccent, pressed ? 0.12f : (hovered ? 0.24f : 0.18f)));
        } else {
          k.Round(r, 15, Rgb(255, 255, 255, hoverAlpha * 0.6f));
          k.Outline(r, 15, Rgb(255, 255, 255, 0.10f));
        }
        k.Text(it.text, chip_.get(), r, it.checked ? accent : secondary);
        break;
      case Kind::AppRow: {
        const float cy = (r.top + r.bottom) / 2;
        if (ID2D1Bitmap* icon = ProgramIcon(k.t, it.detail); icon != nullptr) {
          k.t->DrawBitmap(icon, {r.left + 12, cy - kAppIcon / 2, r.left + 12 + kAppIcon, cy + kAppIcon / 2});
        } else {
          k.Text(it.glyph, glyph_.get(), {r.left + 8, r.top, r.left + 36, r.bottom}, secondary);
        }
        k.Text(it.text, body_.get(), {r.left + 48, r.top, r.right, r.bottom}, primary);
        break;
      }
      case Kind::LogBox: break;  // DrawLog
    }
  }

  // The log's visible lines in `box`: time and level in their own colors,
  // selected lines highlighted, a thumb when there is more to scroll.
  void DrawLog(const Canvas& k, D2D1_RECT_F box, const LogView& log) const {
    const D2D1_COLOR_F primary = Rgb(222, 226, 234);
    const D2D1_COLOR_F secondary = FromColorRef(ui::kSecondaryText);
    if (log.shown.empty()) {
      k.Text(log.lines.empty() ? L"Пока пусто: служба ещё ничего не написала." : L"Ни одной строки этих уровней.",
             button_.get(), box, secondary);
      return;
    }
    k.t->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
    const float view = box.bottom - box.top;
    const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor(log.scroll / kLogLine)));
    for (auto i = static_cast<int>(first); i < log.Count(); ++i) {
      const float top = box.top + static_cast<float>(i) * kLogLine - log.scroll;
      if (top > box.bottom) {
        break;
      }
      const D2D1_RECT_F row{box.left, top, box.right, top + kLogLine};
      if (log.Selected(i)) {
        k.Fill(row, FromColorRef(ui::kAccent, 0.22f));
      }
      // "05:07:30  ERROR  outbound/...": FormatLogLine's columns (main.cpp).
      const std::wstring_view line = log.Line(i);
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
      const float inner = radius - 7;
      k.t->FillEllipse(D2D1::Ellipse(center, inner, inner),
                       k.Color(FromColorRef(ui::kAccent, pressed ? 0.8f : (hovered ? 0.92f : 1.0f))));
      if (c.display != Display::On) {
        k.t->DrawEllipse(D2D1::Ellipse(center, radius - 2, radius - 2), k.Color(ring), 3);
      }
      k.Text(it.glyph, glyphBig_.get(), r, Rgb(12, 20, 36));
    } else {
      const float inner = radius - 7;
      k.t->FillEllipse(D2D1::Ellipse(center, inner, inner),
                       k.Color(Rgb(255, 255, 255, pressed ? 0.05f : (hovered ? 0.10f : 0.06f))));
      k.t->DrawEllipse(D2D1::Ellipse(center, inner, inner), k.Color(ring), 2);
      k.Text(it.glyph, glyphBig_.get(), r, FromColorRef(ui::kPrimaryText));
    }
  }

  wil::com_ptr<ID2D1Factory> d2d_;
  wil::com_ptr<IDWriteFactory> dwrite_;
  wil::com_ptr<IWICImagingFactory> wic_;
  wil::com_ptr<IDWriteInlineObject> ellipsis_;
  wil::com_ptr<IDWriteTextFormat> title_;
  wil::com_ptr<IDWriteTextFormat> state_;
  wil::com_ptr<IDWriteTextFormat> delay_;
  wil::com_ptr<IDWriteTextFormat> heading_;
  wil::com_ptr<IDWriteTextFormat> body_;
  wil::com_ptr<IDWriteTextFormat> caption_;
  wil::com_ptr<IDWriteTextFormat> button_;
  wil::com_ptr<IDWriteTextFormat> center_;
  wil::com_ptr<IDWriteTextFormat> chip_;
  wil::com_ptr<IDWriteTextFormat> wrap_;
  wil::com_ptr<IDWriteTextFormat> captionWrap_;
  wil::com_ptr<IDWriteTextFormat> mono_;
  wil::com_ptr<IDWriteTextFormat> glyph_;
  wil::com_ptr<IDWriteTextFormat> glyphBig_;
  // ProgramIcon's, by exe path; tied to one render target (ForgetIcons).
  mutable std::map<std::wstring, wil::com_ptr<ID2D1Bitmap>> icons_;
  mutable wil::com_ptr<ID2D1Bitmap> flags_;  // Flags'
  mutable bool flagsLoaded_ = false;
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
  std::string openProfile;  // the id of the configuration UiPage::Profile shows
  float scroll = 0;
  Layout layout;
  Interaction in;
  LogView log;
  bool selecting = false;      // dragging a selection over the log
  std::optional<POINT> mouse;  // the cursor over the client area, in pixels
  bool placed = false;  // sized and put by the tray once, on the first show

  wil::com_ptr<ID2D1HwndRenderTarget> target;
  ULONGLONG deactivatedAt = 0;  // GetTickCount64 when the window last lost the focus

  Impl(HINSTANCE inst, CommandHandler handler) : instance(inst), onCommand(std::move(handler)) {
    WNDCLASSW wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Impl::WindowProc;
    wc.hInstance = instance;
    // IDC_ARROW (32512) spelled out: the macro goes through the UNICODE-dependent
    // MAKEINTRESOURCE, which the CI's clang won't match with LoadCursorW.
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
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
      if (item.scrolls && (y < layout.viewTop || y >= layout.viewBottom)) {
        continue;
      }
      return static_cast<int>(i);
    }
    return -1;
  }

  bool InLog(POINT px) const { return layout.logBox && Contains(*layout.logBox, ToDip(px.x), ToDip(px.y)); }

  // The log line under a point, clamped to the lines there are; -1 if none.
  int LogLineAt(POINT px) const {
    if (!layout.logBox || log.shown.empty()) {
      return -1;
    }
    const float y = ToDip(px.y) - layout.logBox->top + log.scroll;
    const auto line = static_cast<int>(std::floor(y / kLogLine));
    return std::clamp(line, 0, log.Count() - 1);
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
    LogState state;
    state.paused = log.paused;
    state.held = log.held.size();
    state.counts = log.counts;
    state.levels = log.levels;
    if (log.HasSelection()) {
      state.detail = log.Line(log.caret);
    }
    layout = painter.Build(content, page, openProfile, size.width, size.height, scroll, state);
    ClampLog();
    in.hover = mouse ? HitAt(*mouse) : -1;
    if (in.focus >= static_cast<int>(layout.items.size()) ||
        (in.focus >= 0 && !Interactive(layout.items[static_cast<std::size_t>(in.focus)]))) {
      in.focus = -1;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  // An edit box over the item for `anchor` and `index` on the page shown
  // (RenameProfile: over the title).
  void EditOver(UiCommand anchor, int index, UiCommand onEnter, const std::wstring& initial, bool digitsOnly) {
    for (const Item& it : layout.items) {
      const bool title = anchor == UiCommand::RenameProfile && it.kind == Kind::Title;
      const bool button = it.action == ItemAction::Command && it.command == anchor && it.index == index;
      if (title || button) {
        BeginEdit(it.rect, onEnter, index, initial, digitsOnly);
        return;
      }
    }
  }

  void Go(UiPage p) {
    EndEdit(true);
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
      case ItemAction::EditName:
        if (item.index >= 0 && static_cast<std::size_t>(item.index) < content.profiles.size()) {
          EditOver(UiCommand::RenameProfile, item.index, UiCommand::RenameProfile,
                   content.profiles[static_cast<std::size_t>(item.index)].name, false);
        }
        return;
      case ItemAction::OpenProfile:
        if (item.index >= 0 && static_cast<std::size_t>(item.index) < content.profiles.size()) {
          openProfile = content.profiles[static_cast<std::size_t>(item.index)].id;
          Go(UiPage::Profile);
        }
        return;
      case ItemAction::ToggleLevel:
        log.levels[static_cast<std::size_t>(item.index)] = !log.levels[static_cast<std::size_t>(item.index)];
        log.anchor = log.caret = -1;  // positions among the shown: they mean other lines now
        log.Rebuild();
        ClampLog();
        Relayout();
        return;
      case ItemAction::PauseLogs:
        log.paused = !log.paused;
        if (!log.paused) {
          AddLogs(std::exchange(log.held, {}), false);
        }
        Relayout();
        return;
      case ItemAction::SaveLogs: SaveLogs(); return;
      case ItemAction::Command: break;
    }
    UiArgs args;
    args.index = item.index;
    args.sub = item.sub;
    args.owner = hwnd;
    const float scale = static_cast<float>(Dpi()) / 96.0f;
    POINT anchor{static_cast<LONG>(item.rect.left * scale), static_cast<LONG>(item.rect.bottom * scale)};
    ClientToScreen(hwnd, &anchor);
    args.anchor = anchor;
    onCommand(item.command, args);
  }

  // The shown lines, selected ones only or all, as text (CRLF).
  std::wstring LogText(bool selectedOnly) const {
    std::wstring text;
    for (int i = 0; i < log.Count(); ++i) {
      if (!selectedOnly || log.Selected(i)) {
        text += log.Line(i);
        text += L"\r\n";
      }
    }
    return text;
  }

  // Ctrl+C: the selected lines, or all shown if none are.
  void CopyLogs() { CopyToClipboard(hwnd, LogText(log.HasSelection())); }

  void SelectAllLogs() {
    if (log.Count() > 0) {
      log.anchor = 0;
      log.caret = log.Count() - 1;
      Relayout();
    }
  }

  // The shown lines into a text file the user picks (UTF-8, CRLF).
  void SaveLogs() {
    std::wstring path;
    try {
      const auto dialog = wil::CoCreateInstance<IFileSaveDialog>(CLSID_FileSaveDialog);
      const COMDLG_FILTERSPEC filter{L"Текст", L"*.txt"};
      THROW_IF_FAILED(dialog->SetFileTypes(1, &filter));
      THROW_IF_FAILED(dialog->SetDefaultExtension(L"txt"));
      SYSTEMTIME now{};
      GetLocalTime(&now);
      const std::wstring name = std::format(L"sovereign-log-{:04}{:02}{:02}-{:02}{:02}.txt", now.wYear, now.wMonth,
                                            now.wDay, now.wHour, now.wMinute);
      THROW_IF_FAILED(dialog->SetFileName(name.c_str()));
      const HRESULT shown = dialog->Show(hwnd);
      if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
      }
      THROW_IF_FAILED(shown);
      wil::com_ptr<IShellItem> item;
      THROW_IF_FAILED(dialog->GetResult(&item));
      wil::unique_cotaskmem_string chosen;
      THROW_IF_FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen));
      path = chosen.get();
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("the save dialog failed");
      return;
    }
    const std::wstring text = LogText(false);
    std::string utf8;
    if (!text.empty()) {
      const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                        nullptr);
      utf8.resize(static_cast<std::size_t>(n));
      WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
    }
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out << utf8;
    if (!out) {
      MessageBoxW(hwnd, L"Не удалось записать файл.", L"Sovereign", MB_OK | MB_ICONWARNING);
    }
  }

  // Right-click on the log: what can be done with its lines. A click on a
  // line outside the selection selects it first.
  void LogMenu(POINT screen) {
    POINT client = screen;
    ScreenToClient(hwnd, &client);
    if (const int line = LogLineAt(client); line >= 0 && InLog(client) && !log.Selected(line)) {
      log.anchor = log.caret = line;
      Relayout();
    }
    wil::unique_hmenu menu(CreatePopupMenu());
    if (!menu) {
      return;
    }
    const UINT any = log.Count() > 0 ? MF_STRING : MF_STRING | MF_GRAYED;
    AppendMenuW(menu.get(), log.HasSelection() ? MF_STRING : MF_STRING | MF_GRAYED, kLogMenuCopy,
                L"Копировать\tCtrl+C");
    AppendMenuW(menu.get(), any, kLogMenuCopyAll, L"Копировать всё");
    AppendMenuW(menu.get(), any, kLogMenuSelectAll, L"Выделить всё\tCtrl+A");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), any, kLogMenuSave, L"Сохранить в файл…");
    AppendMenuW(menu.get(), log.lines.empty() ? MF_STRING | MF_GRAYED : MF_STRING, kLogMenuClear, L"Очистить");
    const auto command = static_cast<UINT>(TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                          screen.x, screen.y, 0, hwnd, nullptr));
    switch (command) {
      case kLogMenuCopy: CopyToClipboard(hwnd, LogText(true)); break;
      case kLogMenuCopyAll: CopyToClipboard(hwnd, LogText(false)); break;
      case kLogMenuSelectAll: SelectAllLogs(); break;
      case kLogMenuSave: SaveLogs(); break;
      case kLogMenuClear:
        // The window's copy only: the service keeps its own, the next lines come.
        log.lines.clear();
        log.held.clear();
        log.anchor = log.caret = -1;
        log.follow = true;
        log.Rebuild();
        ClampLog();
        Relayout();
        break;
      default: break;
    }
  }

  void AddLogs(const std::vector<std::wstring>& lines, bool replace) {
    if (replace) {
      log.lines.clear();
      log.held.clear();
      log.anchor = log.caret = -1;
      log.follow = true;
    } else if (log.paused) {
      // Waits for the resume; the oldest go past what the log keeps.
      log.held.insert(log.held.end(), lines.begin(), lines.end());
      if (log.held.size() > kLogKeep) {
        log.held.erase(log.held.begin(), log.held.end() - static_cast<std::ptrdiff_t>(kLogKeep));
      }
      if (page == UiPage::Logs && IsWindowVisible(hwnd)) {
        Relayout();  // the "+N"
      }
      return;
    }
    for (const std::wstring& line : lines) {
      log.lines.push_back(line);
    }
    int shiftShown = 0;
    if (log.lines.size() > kLogKeep) {
      // The oldest go; what the user looks at and selected stays in place.
      const std::size_t cut = log.lines.size() - kLogKeep;
      shiftShown = static_cast<int>(std::count_if(log.shown.begin(), log.shown.end(),
                                                  [&](int i) { return static_cast<std::size_t>(i) < cut; }));
      log.lines.erase(log.lines.begin(), log.lines.begin() + static_cast<std::ptrdiff_t>(cut));
    }
    log.Rebuild();
    if (shiftShown > 0) {
      log.scroll = std::max(0.0f, log.scroll - static_cast<float>(shiftShown) * kLogLine);
      if (log.HasSelection() || log.anchor >= 0) {
        log.anchor = std::max(log.anchor - shiftShown, 0);
        log.caret = std::max(log.caret - shiftShown, 0);
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
    EndEdit(true);
    scroll += dip;
    Relayout();
  }

  /* ---- typing in place ---- */

  struct InlineEdit {
    wil::unique_hwnd box;
    UiCommand onEnter = UiCommand::RenameProfile;
    int index = 0;
  };
  InlineEdit edit;
  wil::unique_hfont editFont;
  wil::unique_hbrush editBrush;

  void BeginEdit(D2D1_RECT_F rect, UiCommand onEnter, int index, const std::wstring& initial, bool digitsOnly) {
    EndEdit(false);
    const UINT dpi = Dpi();
    const int fontHeight = Scale(17, dpi);
    editFont.reset(CreateFontW(-fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI"));
    if (!editBrush) {
      editBrush.reset(CreateSolidBrush(ui::kCardColor));
    }
    const int left = Scale(rect.left, dpi);
    const int right = Scale(rect.right, dpi);
    const int height = fontHeight + Scale(10, dpi);
    const int top = Scale((rect.top + rect.bottom) / 2, dpi) - height / 2;
    edit.box.reset(CreateWindowExW(0, L"EDIT", initial.c_str(),
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (digitsOnly ? ES_NUMBER : 0), left, top,
                                   right - left, height, hwnd, nullptr, instance, nullptr));
    if (!edit.box) {
      return;
    }
    edit.onEnter = onEnter;
    edit.index = index;
    SendMessageW(edit.box.get(), WM_SETFONT, reinterpret_cast<WPARAM>(editFont.get()), TRUE);
    SendMessageW(edit.box.get(), EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Scale(8, dpi), Scale(8, dpi)));
    SendMessageW(edit.box.get(), EM_LIMITTEXT, 64, 0);
    SendMessageW(edit.box.get(), EM_SETSEL, 0, -1);
    SetWindowSubclass(edit.box.get(), EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetFocus(edit.box.get());
  }

  // The box gone; with keep, what was typed goes to the tray.
  void EndEdit(bool keep) {
    if (!edit.box) {
      return;
    }
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(edit.box.get())) + 1, L'\0');
    text.resize(static_cast<std::size_t>(GetWindowTextW(edit.box.get(), text.data(), static_cast<int>(text.size()))));
    const UiCommand command = edit.onEnter;
    const int index = edit.index;
    edit.box.reset();  // its WM_KILLFOCUS finds no box: nothing twice
    SetFocus(hwnd);
    const auto first = text.find_first_not_of(L" \t");
    if (!keep || first == std::wstring::npos) {
      return;
    }
    UiArgs args;
    args.index = index;
    args.owner = hwnd;
    args.text = text.substr(first, text.find_last_not_of(L" \t") - first + 1);
    onCommand(command, args);
  }

  static LRESULT CALLBACK EditProc(HWND box, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<Impl*>(data);  // NOLINT(performance-no-int-to-ptr) - the subclass's reference data
    switch (message) {
      case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
          self->EndEdit(wParam == VK_RETURN);
          return 0;
        }
        break;
      case WM_CHAR:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
          return 0;  // no beep
        }
        break;
      case WM_KILLFOCUS:
        PostMessageW(self->hwnd, kEndEditMessage, 0, 0);
        break;
      case WM_NCDESTROY:
        RemoveWindowSubclass(box, EditProc, 1);
        break;
      default:
        break;
    }
    return DefSubclassProc(box, message, wParam, lParam);
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
        EndEdit(true);
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
      painter.ForgetIcons();
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
        } else {
          ScrollBy(step * kWheelStep);
        }
        return 0;
      }
      case WM_KEYDOWN: return Key(wParam) ? 0 : DefWindowProcW(w, message, wParam, lParam);
      case WM_CONTEXTMENU:
        if (page == UiPage::Logs && layout.logBox) {
          POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
          if (at.x == -1 && at.y == -1) {
            // From the keyboard (Shift+F10): at the log's corner.
            const float scale = static_cast<float>(Dpi()) / 96.0f;
            at = {static_cast<LONG>(layout.logBox->left * scale) + 16, static_cast<LONG>(layout.logBox->top * scale) + 16};
            ClientToScreen(w, &at);
          }
          LogMenu(at);
          return 0;
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
          deactivatedAt = GetTickCount64();
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case kEndEditMessage:
        if (edit.box && GetFocus() != edit.box.get()) {
          EndEdit(true);
        }
        return 0;
      case WM_CTLCOLOREDIT:
        if (edit.box && reinterpret_cast<HWND>(lParam) == edit.box.get()) {  // NOLINT(performance-no-int-to-ptr)
          const auto dc = reinterpret_cast<HDC>(wParam);  // NOLINT(performance-no-int-to-ptr)
          SetTextColor(dc, ui::kPrimaryText);
          SetBkColor(dc, ui::kCardColor);
          return reinterpret_cast<LRESULT>(editBrush.get());
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_CLOSE:
        // Only the view closes: the tray keeps running.
        EndEdit(true);
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
    if (onLog && ctrl && key == 'A') {
      SelectAllLogs();
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
    if (key == VK_ESCAPE) {
      // Back: from a configuration to the list, from a page to the overview; from there, away.
      if (page == UiPage::Profile) {
        Go(UiPage::Subscription);
      } else if (page != UiPage::Overview) {
        Go(UiPage::Overview);
      } else {
        ShowWindow(hwnd, SW_HIDE);
      }
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
    }
    target->BeginDraw();
    painter.Draw(target.get(), layout, content, in, log);
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
      target.reset();
      painter.ForgetIcons();
    }
  }

  // Sized for the monitor under the cursor and put in the corner of its work
  // area - where the notification area usually is, and the tray is where
  // it's opened from.
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
    const int margin = Scale(12, dpiX);
    SetWindowPos(hwnd, nullptr, std::max(work.left, work.right - w - margin), std::max(work.top, work.bottom - h - margin),
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

void MainWindow::Toggle() {
  Impl& m = *impl_;
  // A click on the tray icon takes the focus from the window before the
  // click arrives: a window that just lost it was in front.
  const bool inFront = GetForegroundWindow() == m.hwnd || GetTickCount64() - m.deactivatedAt < kToggleGraceMs;
  if (IsVisible() && inFront) {
    Hide();
  } else {
    Show(IsVisible() ? m.page : UiPage::Overview);
  }
}

bool MainWindow::IsVisible() const { return impl_->hwnd != nullptr && IsWindowVisible(impl_->hwnd) != FALSE; }

void MainWindow::Update(const UiContent& content) {
  impl_->content = content;
  if (IsVisible()) {
    impl_->Relayout();
  }
}

void MainWindow::EditInPlace(UiCommand anchor, int index, UiCommand onEnter, const std::wstring& initial,
                             bool digitsOnly) {
  impl_->EditOver(anchor, index, onEnter, initial, digitsOnly);
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
  log.Rebuild();
  LogState state;
  state.counts = log.counts;
  if (log.Count() > 2) {
    // Shows what a selection looks like, and the selected line in full.
    log.anchor = 1;
    log.caret = 2;
    state.detail = log.Line(log.caret);
  }
  float scroll = 0;
  const float scale = 96.0f / dpi;
  const std::string profile = content.profiles.empty() ? std::string() : content.profiles.front().id;
  const Layout layout = painter.Build(content, page, profile, static_cast<float>(width) * scale,
                                      static_cast<float>(height) * scale, scroll, state);
  target->BeginDraw();
  painter.Draw(target.get(), layout, content, Interaction{}, log);
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
