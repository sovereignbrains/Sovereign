#include "main_window.h"

#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "cube.h"
#include "diagnostics.h"
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
constexpr float kDefaultWidth = 420;
constexpr float kDefaultHeight = 660;
constexpr float kMinWidth = 360;
constexpr float kMinHeight = 440;
constexpr float kPad = 16;         // the page's margins
constexpr float kCaption = 36;     // our own title bar (Painter::Caption): the system's is gone
constexpr float kMaxPage = 560;    // wider windows keep the page at this, centered
constexpr float kRow = 44;         // a list row
constexpr float kSettingsRow = 34;  // a row of the settings: at 44 they did not fit a screen (30.10.2026, his: "не помещаются")
constexpr float kLatencyRow = 56;  // a server to pick: its name and latency, what the measurement is made of
constexpr float kProfileRow = 54;  // a configuration's row: its name and its state
constexpr float kPillPad = 8;      // a Pill's text from its edge, each side
constexpr float kPillHeight = 20;  // and its height (law.md: 20)
constexpr float kCheckRow = 42;    // a check: its title and "?", its result on the right
constexpr float kSwitchRow = 48;   // a switch whose explanation is in its "?"
constexpr float kHint = 18;        // a "?" that shows a tip when pointed at

// The checks with a button of their own, by their place in UiContent::checks (main.cpp ContentFrom).
constexpr std::size_t kCheckSpeed = 2;
constexpr std::size_t kCheckRoute = 3;
constexpr float kRouteButton = 100;  // the route's "Проверить", a word, not an arrow
constexpr float kSpeedGraph = 150;  // the speed test's graph under the checks
constexpr float kServerRow = 60;   // a server on a configuration's page: its name, protocol and switch
constexpr float kButton = 34;
constexpr float kGap = 8;          // between cards (the law's 8)
constexpr float kSettingsGap = 8;   // between the settings' cards
constexpr float kRadius = 8;
constexpr float kWheelStep = 64;
constexpr float kAppIcon = 20;     // a program's icon in the per-app list
constexpr float kTag = 28;         // a thing on the routing board
constexpr ULONGLONG kToggleGraceMs = 400;  // a tray click right after the window lost focus hides it

// The log page: a line's height, and how many lines it keeps (the tray keeps
// no more either).
constexpr float kLogLine = 20;
constexpr std::size_t kLogKeep = 5000;
// Its scrollbar: the strip at its right edge a press takes - wider than the
// thumb drawn. The middle button's scrolling: a frame, the still zone round
// where it was pressed, and held this long (or moved) it ends on release.
constexpr float kLogBar = 14;
constexpr UINT_PTR kPanTimer = 3;
constexpr UINT kPanFrameMs = 16;
constexpr float kPanStill = 10;
constexpr ULONGLONG kPanHoldMs = 300;

// The log's right-click menu.
// The edit box typed in lost the focus: keep what's in it (posted - the box
// can't be destroyed inside its own message).
constexpr UINT kEndEditMessage = WM_APP + 41;

constexpr UINT kLogMenuCopy = 1;
constexpr UINT kLogMenuCopyAll = 2;
constexpr UINT kLogMenuSelectAll = 3;
constexpr UINT kLogMenuSave = 4;
constexpr UINT kLogMenuClear = 5;

// A right-click menu's items, on anything but the log (ItemMenu).
constexpr UINT kMenuOpen = 101;
constexpr UINT kMenuTestDelays = 103;
constexpr UINT kMenuCopyName = 104;
constexpr UINT kMenuCopyIp = 105;
constexpr UINT kMenuToggle = 106;
constexpr UINT kMenuRefresh = 107;
constexpr UINT kMenuCopyLink = 108;
constexpr UINT kMenuRename = 109;
constexpr UINT kMenuRemove = 110;
constexpr UINT kMenuRun = 111;
constexpr UINT kMenuCopyResult = 112;
constexpr UINT kMenuCopyAll = 113;
constexpr UINT kMenuToggleExitIp = 114;
constexpr UINT kMenuCopyValue = 115;
constexpr UINT kMenuRefreshDirect = 116;
constexpr UINT kMenuHysteriaSpeed = 120;  // + its index in kHysteriaSpeeds

// A tip (TipText) shows when its item is pointed at this long; a "?"'s at once.
constexpr UINT_PTR kTipTimer = 1;
constexpr UINT kTipDelayMs = 500;
// The connect cube's motion (cube.h): frames while it moves, a turn into a
// new state's pose, the turning while it connects (degrees per ms).
constexpr UINT_PTR kCubeTimer = 2;
constexpr UINT kCubeFrameMs = 15;
constexpr UINT kCubeIdleFrameMs = 33;  // the idle life (Idle) is slow: 30 frames a second are plenty
constexpr ULONGLONG kCubeTurnMs = 520;
constexpr UINT kQuestionInMs = 170;  // MainWindow::Ask's card coming in
constexpr UINT kFoldMs = 240;        // a fold opening or closing (law.md --d-fold)
constexpr float kCubeSpin = 0.12f;

// Segoe Fluent Icons (Windows 11; the same code points in Segoe MDL2 Assets).
constexpr const wchar_t* kGlyphSync = L"\xE895";
constexpr const wchar_t* kGlyphBack = L"\xE72B";
constexpr const wchar_t* kGlyphChevron = L"\xE76C";
constexpr const wchar_t* kGlyphChevronDown = L"\xE70D";  // a fold that opens
constexpr const wchar_t* kGlyphChevronUp = L"\xE70E";    // ...and closes
constexpr const wchar_t* kGlyphLog = L"\xE8A5";
constexpr const wchar_t* kGlyphSettings = L"\xE713";
constexpr const wchar_t* kGlyphClose = L"\xE8BB";  // ChromeClose
constexpr const wchar_t* kGlyphMinimize = L"\xE921";  // ChromeMinimize
constexpr const wchar_t* kGlyphPower = L"\xE7E8";
constexpr const wchar_t* kGlyphPaste = L"\xE77F";
constexpr const wchar_t* kGlyphRefresh = L"\xE72C";
constexpr const wchar_t* kGlyphRemove = L"\xE711";
constexpr const wchar_t* kGlyphAdd = L"\xE710";
constexpr const wchar_t* kGlyphFile = L"\xE8E5";
constexpr const wchar_t* kGlyphFolder = L"\xE8B7";
constexpr const wchar_t* kGlyphCopy = L"\xE8C8";
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
constexpr const wchar_t* kGlyphRoute = L"\xE8F0";
constexpr const wchar_t* kGlyphShield = L"\xEA18";
constexpr const wchar_t* kGlyphSpeed = L"\xEC4A";
constexpr const wchar_t* kGlyphReport = L"\xEBE8";  // Bug: "something's wrong", plainer than the feedback bubble

// The three ways to add a configuration, each a key of its own with its tip
// where there was one "+" with a list dropping from it (his, 30.10.2026:
// 'вместо кнопки добавить и плюсика эти 3 расположить и подсказку на каждую').
struct AddWay {
  const wchar_t* name;
  const wchar_t* glyph;
  UiCommand command;
  const wchar_t* tip;
};
const std::array<AddWay, 3> kAddWays = {{
    {L"Вставить из буфера", kGlyphPaste, UiCommand::PasteSubscription,
     L"Вставить из буфера: ссылка подписки, ключ или конфиг"},
    {L"QR-код с экрана", kGlyphQrCode, UiCommand::ScanScreen, L"QR-код с экрана: найти его и добавить подписку"},
    {L"Из файла…", kGlyphFile, UiCommand::ImportFile, L"Из файла: конфиг, ключи или картинка с QR-кодом"},
}};

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
  ServerCard,    // the server in use: flag and name, its network and latency; opens the servers
  Banner,        // a one-line call to action, lit (`checked`: what's wrong, amber)
  Button,        // a normal button
  AccentButton,  // the page's main action
  DangerButton,  // quit
  IconButton,    // a square button with a glyph; `checked`: a dot on it (something new there)
  Switch,        // a row with a toggle switch
  Choice,        // a pickable row with a radio mark
  ProfileRow,    // a configuration: name, its state under it (`warn`: in red), dimmed when off (!`checked`)
  Toggle,        // just a switch, no text (`checked`: on)
  CheckRow,      // a check: a dot in its status' colour (index: UiCheck::Status), title, result, what it means
  Segment,       // half of a two-way switch
  Chip,          // an on/off filter: tinted when on
  AppRow,        // a program in the per-app list
  LogBox,        // the log's lines
  Hint,          // a "?": pointed at, its text shows in a tip
  ValueRow,      // a setting with its value on the right and a chevron: a click picks another
  Meter,         // a thin bar: how much is used (index: per mille)
  Tag,           // a thing on the routing board: a program's icon (text2: its exe), its name, a detail after it
  AppTitle,      // our title bar's name, after the app's icon (text2: the exe it's taken from)
  SpeedGraph,    // the speed test's throughput over time, down and up (index: its check's, UiCheck speedDown/Up)
  Question,      // MainWindow::Ask's card, the window dimmed round it (index: its UiQuestion::Tone)
  Pill,          // a small label in a rounded outline, not a button (`warn`: amber)
  Day,           // a calendar's key on a question's card: a day (text: its number, checked: chosen, sub 1: today) or a step (glyph set, sub: -1 back, 1 on)
};

// EditName: the configuration `index`'s name, typed over the page's title.
// Hint: nothing on a click - it's there to be pointed at.
enum class ItemAction : std::uint8_t {
  None,
  Command,
  Page,
  OpenProfile,
  EditName,
  ToggleLevel,
  PauseLogs,
  SaveLogs,
  ToggleMore,  // the configurations' relay fold, folded or not (the window's own)
  Hide,        // our title bar's cross: the window away to the tray
  Hint,
  Answer       // a question's button (index: 1 yes, 0 no); while one is open, the only ones that take a click
};

struct Item {
  Kind kind = Kind::Text;
  D2D1_RECT_F rect{};
  std::wstring_view glyph;  // empty: none (never a null pointer - /analyze)
  std::wstring text;
  std::wstring detail;
  std::wstring text2;  // a third line where a kind has one (CheckRow: what the result means)
  std::wstring tip;    // what's in it and what a click does, where its label doesn't say: shown pointed at a while
  std::string logo;    // a Tag's service logo (icons.h ServiceLogo): the service's id; a ValueRow's, before its title
  ItemAction action = ItemAction::None;
  UiCommand command = UiCommand::Toggle;
  int index = 0;
  int sub = 0;  // a second index for the command: a server within a configuration
  bool checked = false;
  bool enabled = true;
  bool warn = false;    // something's wrong with it (a ProfileRow's refresh failed)
  bool glass = false;   // a Card on the main screen: see-through - the light and the streams behind it show
  bool scrolls = true;  // lives in the page (scrolls, clipped), not in the rail
  bool hinted = false;  // a ValueRow with a "?" after its title: the value starts after that
  bool key = false;     // an IconButton drawn as a key with height (DrawKey), not flat
  bool chrome = false;  // an IconButton of the title bar (DrawChrome): its pill inset from the bar's edges
  bool engraved = false;  // a Muted label set as engraved capitals (engraved_)
  int rail = -1;        // a Card or Pill of the routing board: the way it's of (a bucket, BucketColor); -1: none
  bool quiet = false;   // a Card with a rail: only the rail, no panel - an empty way, a line to find
  int tick = -1;        // a Switch or ValueRow of known fate (a bucket, BucketColor): its mark on the card's edge; -1: none
  bool tickAlways = false;  // the mark stays with the switch off (a DNS way); else it burns while the switch is on
  float fold = -1;      // a fold's handle: how open it is (0..1), its chevron turned by it; -1: not a fold's
  float clip = -1;      // a fold's row while the fold moves: seen and clicked above this line only; -1: whole
};

// What of an item is seen: its rectangle, a moving fold's row cut at the fold's foot.
inline D2D1_RECT_F Seen(const Item& it) {
  D2D1_RECT_F r = it.rect;
  if (it.clip >= 0) {
    r.bottom = std::min(r.bottom, it.clip);
  }
  return r;
}

// How open a fold is: while it moves, the window's number; at rest, its flag.
inline float FoldOf(bool open, float moving) { return moving >= 0 ? moving : (open ? 1.0f : 0.0f); }


// A way's colour on the routing board: a rail down its card's left edge and
// a dot on its labels - the traffic's own (ui_style.h), the cube's blue and
// the dots flying out of it, not a palette of the board's. Dull where it's
// drawn (30.10.2026, his: "менее цвета неоновые"): kRailAlpha. By bucket: 0
// directly, 1 through the VPN, 2 blocked, 3 WARP.
constexpr COLORREF BucketColor(int bucket) {
  switch (bucket) {
    case kBucketDirect: return ui::kDirect;
    case kBucketBlock: return ui::kDanger;
    case kBucketWarp: return ui::kWarp;
    default: return ui::kVpn;
  }
}
constexpr float kRailAlpha = 0.7f;
constexpr float kRailQuiet = 12;  // an empty way's rail: a short stroke, not a pale long one
// One line for every drawn icon - the title bar's, a row's chevron and "+",
// the "?"'s ring, a switch's groove: a family of one weight (30.10.2026, a
// review's "иконки смешаны по стилю").
constexpr float kIconStroke = 1.3f;

// What an item's tip says: a "?"'s text, its own tip, an icon button's name;
// empty: it has none.
std::wstring_view TipText(const Item& it) {
  if (!it.tip.empty()) {
    return it.tip;
  }
  return it.kind == Kind::Hint || it.kind == Kind::IconButton ? std::wstring_view(it.text) : std::wstring_view();
}

struct Layout {
  std::vector<Item> items;
  float contentHeight = 0;  // the page's full height from viewTop, for scrolling
  float scroll = 0;         // how far the page is scrolled
  float viewTop = 0;        // the scrolling viewport, in window DIPs
  float viewBottom = 0;
  std::optional<D2D1_RECT_F> logBox;
  std::optional<D2D1_RECT_F> glow;  // the main screen's switch: a light behind it, under it all (DrawBackdrop)
};

// A fold's body built in full from `from` to `to`, shown as far as the fold has opened: its
// card ends at the foot, the rows over it are cut there and the ones past it are gone.
// Returns the foot. (law.md: the disclosure runs 240 ms, --d-fold.)
inline float FoldBody(Layout& l, std::size_t first, float from, float to, float open) {
  if (open >= 1) {
    return to;
  }
  const float foot = from + (to - from) * std::max(open, 0.0f);
  l.items.erase(std::remove_if(l.items.begin() + static_cast<std::ptrdiff_t>(first), l.items.end(),
                               [&](const Item& it) { return it.rect.top >= foot; }),
                l.items.end());
  for (std::size_t i = first; i < l.items.size(); ++i) {
    if (l.items[i].rect.bottom > foot) {
      l.items[i].clip = foot;
    }
  }
  return foot;
}

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
  bool barHot = false;  // the pointer on the scrollbar, or dragging it: drawn wider
  std::optional<D2D1_POINT_2F> pan;  // the middle button's scrolling: where it was pressed

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

// The log's scrollbar thumb in `box` - its strip's width, the drawn part is
// thinner; none while every line fits.
std::optional<D2D1_RECT_F> LogThumb(D2D1_RECT_F box, const LogView& log) {
  const float view = box.bottom - box.top;
  if (log.Height() <= view + 1) {
    return std::nullopt;
  }
  const float thumb = std::max(32.0f, view * view / log.Height());
  const float top = box.top + (view - thumb) * (log.scroll / (log.Height() - view));
  return D2D1_RECT_F{box.right - kLogBar, top, box.right, top + thumb};
}

// What the log page's layout shows of the log's state.
struct LogState {
  bool paused = false;
  std::size_t held = 0;
  std::array<int, kLevelGroups> counts{};
  std::array<bool, kLevelGroups> levels{true, true, true, true};
  std::wstring detail;  // the caret's line in full: the list cuts long ones
};

// The live traffic round the main screen's switch (UiContent::flow*): dots
// along paths - up through the cube for the VPN (WARP's in its own colour),
// round its side for what goes directly - as many and as fast as the rates
// say; a blocked ad flies at the cube and breaks on it in red sparks. Moved
// a frame a screen refresh while the overview shows (Impl::liveAt), never
// with Windows' animations off.
enum class Stream : std::uint8_t { Vpn, Warp, Direct, Blocked, Spark };

struct Particle {
  Stream stream = Stream::Vpn;
  float t = 0;         // 0..1 along its path; a spark: its age, seconds
  float speed = 0;     // t a second
  float offset = 0;    // -1..1 across its path
  float size = 2;      // DIPs
  D2D1_POINT_2F at{};  // a spark: where it broke off
  D2D1_POINT_2F v{};   // a spark: DIPs a second
};

struct LiveTraffic {
  std::vector<Particle> particles;
  std::array<float, 3> owed{};  // dots due a stream (Vpn, Warp, Direct), the fraction kept
  std::uint64_t blockedSeen = 0;
  bool blockedKnown = false;
  std::uint32_t seed = 0x2545F491u;

  float Random() {  // 0..1
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / 16777216.0f;
  }
};

constexpr std::size_t kMaxParticles = 600;
constexpr float kSparkLife = 0.6f;

// Dots a second for `rate` bytes a second: a few for a trickle, a river for
// a video or two (about 130 at 1 MB/s, 160 at most); none for nothing.
float DotsPerSecond(double rate) {
  return rate < 64 ? 0.0f : std::min(160.0f, 3 + 26 * std::log2(1 + static_cast<float>(rate) / 32768));
}

// A dot's size: bigger as the stream swells, DIPs.
float DotSize(double rate, float random) {
  return 1.2f + 1.2f * random + std::min(1.2f, std::log2(1 + static_cast<float>(rate) / 262144) * 0.5f);
}

// How fast: a trip of a little over two seconds, quicker as the rate grows.
float DotSpeed(double rate) {
  return 0.42f + 0.3f * std::min(1.0f, std::log2(1 + static_cast<float>(rate) / 65536) / 5);
}

// Where a dot is, round the switch's square `cube`: from under it up to over it.
D2D1_POINT_2F DotAt(const Particle& p, D2D1_RECT_F cube) {
  const float s = cube.right - cube.left;
  const D2D1_POINT_2F c{(cube.left + cube.right) / 2, (cube.top + cube.bottom) / 2};
  const float bottom = c.y + s * 1.1f;
  const float top = c.y - s * 1.1f;
  switch (p.stream) {
    case Stream::Vpn:
    case Stream::Warp: {
      // Up through the cube, drawn in to its middle on the way.
      const float squeeze = 1 - 0.75f * std::sin(std::numbers::pi_v<float> * std::clamp(p.t, 0.0f, 1.0f));
      const float lane = p.stream == Stream::Warp ? s * 0.07f : 0.0f;
      return {c.x + lane + p.offset * s * 0.22f * squeeze, bottom + (top - bottom) * p.t};
    }
    case Stream::Direct: {
      // Up round the cube's side - its own lane past the glass.
      const float side = p.offset < 0 ? -1.0f : 1.0f;
      const D2D1_POINT_2F a{c.x + p.offset * s * 0.18f, bottom};
      const D2D1_POINT_2F m{c.x + side * s * 1.05f, c.y};
      const D2D1_POINT_2F b{c.x + p.offset * s * 0.18f, top};
      const float u = 1 - p.t;
      return {u * u * a.x + 2 * u * p.t * m.x + p.t * p.t * b.x, u * u * a.y + 2 * u * p.t * m.y + p.t * p.t * b.y};
    }
    case Stream::Blocked:
      // Up at the cube, as far as its lower face.
      return {c.x + p.offset * s * 0.2f, bottom + (c.y + s * 0.22f - bottom) * p.t};
    case Stream::Spark:
      return {p.at.x + p.v.x * p.t, p.at.y + p.v.y * p.t + 90 * p.t * p.t};
  }
  return c;
}

// One step of `dt` seconds round `cube`: the dots move, new ones come by the
// rates (none unless `on`), a blocked ad that reached the cube breaks up.
void StepLive(LiveTraffic& live, const UiContent& c, bool on, float dt, D2D1_RECT_F cube) {
  std::vector<Particle> sparks;
  for (Particle& p : live.particles) {
    if (p.stream == Stream::Spark) {
      p.t += dt;
      continue;
    }
    p.t += p.speed * dt;
    if (p.stream == Stream::Blocked && p.t >= 1) {
      Particle hit = p;
      hit.t = 1;
      const D2D1_POINT_2F at = DotAt(hit, cube);
      for (int i = 0; i < 6 && live.particles.size() + sparks.size() < kMaxParticles; ++i) {
        // Down and out, as from a blow from below.
        const float angle = std::numbers::pi_v<float> * (0.1f + 0.8f * live.Random());
        const float speed = 50 + 80 * live.Random();
        sparks.push_back({.stream = Stream::Spark, .t = 0, .speed = 0, .offset = 0, .size = 1.2f + live.Random(),
                          .at = at, .v = {std::cos(angle) * speed, std::sin(angle) * speed * 0.6f - 40}});
      }
    }
  }
  std::erase_if(live.particles,
                [](const Particle& p) { return p.stream == Stream::Spark ? p.t >= kSparkLife : p.t >= 1; });
  live.particles.insert(live.particles.end(), sparks.begin(), sparks.end());

  const auto spawn = [&](Stream stream, double rate) {
    if (live.particles.size() < kMaxParticles) {
      live.particles.push_back({.stream = stream, .t = 0, .speed = DotSpeed(rate) * (0.85f + 0.3f * live.Random()),
                                .offset = live.Random() * 2 - 1, .size = DotSize(rate, live.Random()), .at = {}, .v = {}});
    }
  };
  const std::array<std::pair<Stream, double>, 3> streams = {
      {{Stream::Vpn, c.flowVpn}, {Stream::Warp, c.flowWarp}, {Stream::Direct, c.flowDirect}}};
  for (std::size_t i = 0; i < streams.size(); ++i) {
    live.owed[i] = on ? live.owed[i] + DotsPerSecond(streams[i].second) * dt : 0.0f;
    for (; live.owed[i] >= 1; live.owed[i] -= 1) {
      spawn(streams[i].first, streams[i].second);
    }
  }
  // The ads blocked since the last step: a dot each, a few at most at once.
  if (!live.blockedKnown || c.blockedCount < live.blockedSeen) {
    live.blockedKnown = true;
    live.blockedSeen = c.blockedCount;
  }
  for (int n = 0; on && c.blockedCount > live.blockedSeen && n < 4; ++n) {
    spawn(Stream::Blocked, 0);
    live.particles.back().speed = 0.9f;
  }
  live.blockedSeen = c.blockedCount;
}

// What a paint depends on besides the layout.
struct Interaction {
  int hover = -1;
  int pressed = -1;
  int focus = -1;
  bool focusVisible = false;
  int tip = -1;  // the item whose tip shows: a "?" pointed at, anything else with one (TipText) pointed at a while
  std::optional<CubePose> cube;  // the switch mid-motion (cube.h); none: at rest in its state's pose
};

bool Contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

D2D1_RECT_F Inflate(const D2D1_RECT_F& r, float by) { return {r.left - by, r.top - by, r.right + by, r.bottom + by}; }

// The calendar on a question's card (UiCalendar): dates as days since
// 1970-01-01 in the Gregorian calendar, to move by days and by months.
// A calendar key's Item::index as an answer (below them: 0 no, 1 yes, 2 other).
constexpr int kAnswerMonthBack = 10;
constexpr int kAnswerMonthOn = 11;
constexpr int kAnswerYearBack = 12;
constexpr int kAnswerYearOn = 13;
constexpr int kAnswerDay = 100;  // + the day of the month
constexpr int kCalendarFirstYear = 2000;
constexpr int kCalendarLastYear = 2099;
constexpr std::array<const wchar_t*, 12> kMonthNames = {L"Январь", L"Февраль", L"Март",    L"Апрель",
                                                        L"Май",    L"Июнь",    L"Июль",    L"Август",
                                                        L"Сентябрь", L"Октябрь", L"Ноябрь",  L"Декабрь"};
constexpr std::array<const wchar_t*, 7> kWeekdayNames = {L"пн", L"вт", L"ср", L"чт", L"пт", L"сб", L"вс"};

std::int64_t DaysFromCivil(int year, int month, int day) {
  const int y = year - (month <= 2 ? 1 : 0);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<std::int64_t>(y - era * 400);
  const std::int64_t shifted = month > 2 ? month - 3 : month + 9;
  const std::int64_t doy = (153 * shifted + 2) / 5 + day - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void CivilFromDays(std::int64_t days, int& year, int& month, int& day) {
  const std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

int DaysInMonth(int year, int month) {
  constexpr std::array<int, 12> days = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return days.at(static_cast<std::size_t>(std::clamp(month, 1, 12) - 1)) + (month == 2 && leap ? 1 : 0);
}

// 0 for Monday: 1970-01-01 was a Thursday.
int WeekdayOf(int year, int month, int day) {
  return static_cast<int>(((DaysFromCivil(year, month, day) + 3) % 7 + 7) % 7);
}

void CalendarShiftDays(UiCalendar& c, int days) {
  CivilFromDays(DaysFromCivil(c.year, c.month, c.day) + days, c.year, c.month, c.day);
  c.year = std::clamp(c.year, kCalendarFirstYear, kCalendarLastYear);
  c.day = std::min(c.day, DaysInMonth(c.year, c.month));
}

// A month or a year on: the day stays, or the month's last if it has fewer.
void CalendarShiftMonths(UiCalendar& c, int months) {
  const int total = std::clamp(c.year * 12 + (c.month - 1) + months, kCalendarFirstYear * 12,
                               kCalendarLastYear * 12 + 11);
  c.year = total / 12;
  c.month = total % 12 + 1;
  c.day = std::min(c.day, DaysInMonth(c.year, c.month));
}

std::wstring_view Glyph(const wchar_t* glyph) { return glyph != nullptr ? std::wstring_view(glyph) : std::wstring_view(); }

Item Make(Kind kind, D2D1_RECT_F rect, std::wstring text = {}, const wchar_t* glyph = nullptr) {
  Item i;
  i.kind = kind;
  i.rect = rect;
  i.text = std::move(text);
  i.glyph = Glyph(glyph);
  return i;
}

// What a check's result keeps clear on the right of its row: its button's room
// (Item::checked), or just the edge.
float CheckButtonRoom(const Item& it) {
  if (!it.checked) {
    return 16;  // the line 20 from the card, the row being 4 in
  }
  const bool word = std::cmp_equal(it.sub, kCheckRoute) && it.index == static_cast<int>(UiCheck::Status::NotRun);
  return word ? 16 + kRouteButton + 8 : 44;  // the word's button ends on the line 20; a key's zone on 14
}

Item CommandItem(Kind kind, D2D1_RECT_F rect, std::wstring text, const wchar_t* glyph, UiCommand command, int index = 0) {
  Item i = Make(kind, rect, std::move(text), glyph);
  i.action = ItemAction::Command;
  i.command = command;
  i.index = index;
  return i;
}

// A configuration's report for support; while one waits for its test of the
// servers, says so and takes no clicks.
Item ReportButton(const UiContent& c, bool support, D2D1_RECT_F rect, int index) {
  Item i = CommandItem(Kind::Button, rect,
                       c.reportPending ? L"Проверяю серверы…"
                       : support       ? L"Написать в поддержку"
                                       : L"Отчёт для поддержки",
                       kGlyphDownload, UiCommand::SupportReport, index);
  i.enabled = !c.reportPending;
  i.tip = support ? L"Проверить серверы, сохранить отчёт, открыть поддержку"
                  : L"Проверить серверы и сохранить отчёт для поддержки";
  return i;
}

bool Interactive(const Item& item) { return item.action != ItemAction::None && item.enabled; }

std::wstring StateDetailBase(const UiContent& c);

// The overview's words over the cube - the cube's pose says the state, so
// only what it can't: what's wrong and what to do about it, and when the
// kill switch holds the internet closed, that. Empty while all is well.
std::wstring StateDetail(const UiContent& c) {
  std::wstring detail = StateDetailBase(c);
  if (c.killSwitchActive && c.display != Display::On && c.display != Display::Off) {
    detail += L"\nИнтернет закрыт Kill switch'ем, пока подключение не восстановится.";
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
      // Nothing to run: why (no configuration at all - the first screen says it).
      return c.hasConfig || c.profiles.empty() ? std::wstring() : L"Включи конфигурацию: " + c.combineError;
    case Display::Starting:
    case Display::On: return {};
    case Display::Error: return c.error.empty() ? std::wstring(L"ядро не запустилось") : c.error;
  }
  return {};
}

// What's wrong, each in a few words for the main screen's one line and in
// full, in red, at the top of the log (30.10.2026, his: "эту хуйню красную
// уберем ... ошибки красные в журнале писать"). The subscription's trouble
// last: its report button goes under it.
struct Issue {
  std::wstring brief;
  std::wstring full;
};

std::vector<Issue> Issues(const UiContent& c) {
  std::vector<Issue> issues;
  if (c.display == Display::ServiceDown) {
    issues.push_back({L"Служба Sovereign не отвечает", StateDetail(c)});
  } else if (c.display == Display::Error) {
    issues.push_back({L"Не удалось подключиться", StateDetail(c)});
  }
  if (c.lockedOut) {
    issues.push_back({L"Интернет закрыт Kill switch'ем",
                      L"Интернет закрыт Kill switch'ем: подключиться не удаётся — " + c.lockoutReason +
                          L". Так настоящий IP не утечёт, но и интернета нет."});
  }
  if (!c.killSwitchError.empty()) {
    issues.push_back({L"Kill switch не включился", L"Kill switch: " + c.killSwitchError});
  }
  if (!c.localNetwork.empty()) {
    issues.push_back({L"Сбоит твоя сеть, а не VPN", c.localNetwork});
  }
  if (!c.trouble.empty() && c.troubleProfile >= 0) {
    issues.push_back({L"Проблема с подпиской", c.trouble});
  }
  return issues;
}

bool AnyWaiting(const UiContent& c) {
  return std::any_of(c.profiles.begin(), c.profiles.end(), [](const UiProfile& p) { return p.waiting; });
}

// Where a page's "back" (and Esc) leads: a configuration to the list, the
// checks to the settings, the rest to the overview.
UiPage BackOf(UiPage page) {
  switch (page) {
    case UiPage::Profile: return UiPage::Subscription;
    case UiPage::Checks: return UiPage::Settings;  // its row is there
    default: return UiPage::Overview;  // the routing and the log too: the title bar's keys open them
  }
}

std::wstring UpdateStatus(const UiContent& c) {
  switch (c.update) {
    case UiUpdate::Idle:
      return c.updateMode == UpdateMode::Off       ? L"Обновления: только кнопкой справа."
             : c.updateMode == UpdateMode::Install ? L"Обновления ставятся сами."
                                                   : L"Обновления — раз в 12 часов.";
    case UiUpdate::Checking: return L"Проверяю…";
    case UiUpdate::UpToDate: return L"Это последняя версия.";
    case UiUpdate::Available:
      return c.updateError.empty() ? L"Доступна версия " + c.updateVersion : L"Не удалось: " + c.updateError;
    case UiUpdate::Downloading: return L"Скачиваю версию " + c.updateVersion + L"…";
    case UiUpdate::Waiting: return L"Версия " + c.updateVersion + L" скачана, ждёт паузы.";
    case UiUpdate::Failed: return L"Не удалось проверить: " + c.updateError;
    case UiUpdate::Off: return L"Сборка для разработки: обновления не проверяются.";
  }
  return {};
}

// A flag emoji in a text layout (flags.h FindFlagEmoji), drawn as the
// sprite's flag, 4:3, centered on the text's capitals. Kept next to the
// layout it's in and outliving it, so AddRef and Release count nothing.
// Without a target - a layout only measured - it takes its room and draws
// nothing.
class FlagInline final : public IDWriteInlineObject {
 public:
  FlagInline(float fontSize, std::size_t index, ID2D1RenderTarget* target, ID2D1Bitmap* sprite)
      : fontSize_(fontSize), height_(std::round(fontSize * 0.86f)), index_(index), target_(target), sprite_(sprite) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) {
      *object = static_cast<IDWriteInlineObject*>(this);
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
  ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

  HRESULT STDMETHODCALLTYPE Draw(void* /*context*/, IDWriteTextRenderer* /*renderer*/, FLOAT originX, FLOAT originY,
                                 BOOL /*sideways*/, BOOL /*rightToLeft*/, IUnknown* /*effect*/) noexcept override {
    if (target_ == nullptr || sprite_ == nullptr) {
      return S_OK;
    }
    const auto n = static_cast<float>(index_);
    const float column = std::fmod(n, static_cast<float>(kFlagColumns));
    const float row = std::floor(n / static_cast<float>(kFlagColumns));
    const D2D1_RECT_F source{column * kFlagWidth, row * kFlagHeight, (column + 1) * kFlagWidth,
                             (row + 1) * kFlagHeight};
    const D2D1_RECT_F place{originX + 1, originY, originX + 1 + Width(), originY + height_};
    // A small enamel badge, not a bare rectangle: rounded corners, the flag
    // inset a pixel of the sprite (no neighbour's colour at the edge), a
    // gloss down from the top, a dark hairline round it and a light one inside.
    const D2D1_RECT_F inner{source.left + 1, source.top + 1, source.right - 1, source.bottom - 1};
    const float scaleX = (place.right - place.left) / (inner.right - inner.left);
    const float scaleY = (place.bottom - place.top) / (inner.bottom - inner.top);
    wil::com_ptr<ID2D1BitmapBrush> brush;
    if (FAILED(target_->CreateBitmapBrush(
            sprite_, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
            D2D1::BrushProperties(1.0f, D2D1::Matrix3x2F::Scale(scaleX, scaleY) *
                                            D2D1::Matrix3x2F::Translation(place.left - inner.left * scaleX,
                                                                          place.top - inner.top * scaleY)),
            brush.put()))) {
      target_->DrawBitmap(sprite_, place, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
      return S_OK;
    }
    const float radius = std::max(1.5f, height_ * 0.17f);
    const D2D1_ROUNDED_RECT shape{place, radius, radius};
    target_->FillRoundedRectangle(shape, brush.get());
    wil::com_ptr<ID2D1SolidColorBrush> ink;
    if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), ink.put()))) {
      const D2D1_ROUNDED_RECT light{
          D2D1::RectF(place.left + 0.5f, place.top + 0.5f, place.right - 0.5f, place.bottom - 0.5f),
          radius - 0.5f, radius - 0.5f};
      target_->DrawRoundedRectangle(light, ink.get(), 1.0f);
      ink->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
      const D2D1_ROUNDED_RECT dark{
          D2D1::RectF(place.left - 0.5f, place.top - 0.5f, place.right + 0.5f, place.bottom + 0.5f),
          radius + 0.5f, radius + 0.5f};
      target_->DrawRoundedRectangle(dark, ink.get(), 1.0f);
    }
    wil::com_ptr<ID2D1GradientStopCollection> stops;
    wil::com_ptr<ID2D1LinearGradientBrush> gloss;
    const std::array<D2D1_GRADIENT_STOP, 2> pair{{{0, D2D1::ColorF(1, 1, 1, 0.22f)}, {1, D2D1::ColorF(1, 1, 1, 0)}}};
    if (SUCCEEDED(target_->CreateGradientStopCollection(pair.data(), 2, stops.put())) &&
        SUCCEEDED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({place.left, place.top},
                                                {place.left, place.top + (place.bottom - place.top) * 0.55f}),
            stops.get(), gloss.put()))) {
      target_->FillRoundedRectangle(shape, gloss.get());
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) noexcept override {
    // The flag's middle where the capitals' is: about 0.35 em over the baseline.
    *metrics = {.width = Width() + 3, .height = height_, .baseline = height_ / 2 + fontSize_ * 0.35f,
                .supportsSideways = FALSE};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) noexcept override {
    *overhangs = {};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before,
                                               DWRITE_BREAK_CONDITION* after) noexcept override {
    *before = DWRITE_BREAK_CONDITION_NEUTRAL;
    *after = DWRITE_BREAK_CONDITION_NEUTRAL;
    return S_OK;
  }

 private:
  float Width() const { return std::round(height_ * 4 / 3); }

  float fontSize_;
  float height_;
  std::size_t index_;
  ID2D1RenderTarget* target_;
  ID2D1Bitmap* sprite_;
};

// `text` laid out in `format` with its flag emoji as flags, drawn onto
// `target` from `sprite` (none: measured only). The flags are kept in
// `flags`, which must outlive the layout. Null if DirectWrite fails.
wil::com_ptr<IDWriteTextLayout> MakeTextLayout(IDWriteFactory* dwrite, std::wstring_view text, IDWriteTextFormat* format,
                                               float width, float height, std::vector<FlagInline>& flags,
                                               ID2D1RenderTarget* target = nullptr, ID2D1Bitmap* sprite = nullptr) {
  wil::com_ptr<IDWriteTextLayout> layout;
  if (FAILED(dwrite->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, std::max(width, 0.0f),
                                      std::max(height, 0.0f), layout.put()))) {
    return nullptr;
  }
  const std::vector<FlagEmoji> found = FindFlagEmoji(text);
  flags.clear();
  flags.reserve(found.size());  // no reallocation below: the layout holds their addresses
  for (const FlagEmoji& flag : found) {
    flags.emplace_back(format->GetFontSize(), flag.index, target, sprite);
    layout->SetInlineObject(&flags.back(), {static_cast<UINT32>(flag.at), 4});
  }
  return layout;
}

// Device-independent resources and everything between content and pixels:
// the layout of a page and its drawing onto any Direct2D target - a window's
// or, for snapshots, a WIC bitmap's.
class Painter {
 public:
  // The speed test's graph: how far the line has gone towards its newest
  // sample (0..1) - Impl eases it frame by frame, so the graph grows
  // smoothly, not a quarter of a second at a time (he: "like 10 fps").
  float speedTip = 1;
  // MainWindow::Ask's card coming in (0..1): the dimming deepens, the card
  // grows to its size from a little smaller.
  float questionShown = 1;

  Painter() {
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()));
    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.put())));
    wic_ = wil::CoCreateInstanceNoThrow<IWICImagingFactory>(CLSID_WICImagingFactory);
    fonts_ = LoadFontCollection(dwrite_.get());
    THROW_IF_FAILED(dwrite_->CreateEllipsisTrimmingSign(MakeFormat(Text(), 14).get(), ellipsis_.put()));
    title_ = MakeFormat(Display(), ui::kTTitle, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    state_ = MakeFormat(Display(), ui::kTTitle, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    delay_ = MakeFormat(Text(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    delay_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    heading_ = MakeFormat(Text(), 15.75f, DWRITE_FONT_WEIGHT_SEMI_BOLD);  // 14 x 1.125: the scale caption 12.5 / body 14 / heading 15.75 / title 20
    body_ = MakeFormat(Text(), 14);
    caption_ = MakeFormat(Text(), 12.5f);
    button_ = MakeFormat(Text(), 14);
    button_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    chip_ = MakeFormat(Text(), 12.5f);
    chip_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    center_ = MakeFormat(Text(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    center_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    wrap_ = MakeFormat(Text(), 14, DWRITE_FONT_WEIGHT_NORMAL, true);
    captionWrap_ = MakeFormat(Text(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
    mono_ = MakeFormat(L"Consolas", 12.5f);
    trailing_ = MakeFormat(Text(), 12.5f);
    trailing_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    pill_ = MakeFormat(Text(), 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    engraved_ = MakeFormat(Text(), ui::kTMicro, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hint_ = MakeFormat(Text(), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hint_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    tip_ = MakeFormat(Text(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
    ask_ = MakeFormat(Display(), ui::kTLead, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    const std::wstring glyphs = ui::GlyphFamily(dwrite_.get());
    glyph_ = MakeFormat(glyphs.c_str(), 16);
    glyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    THROW_IF_FAILED(d2d_->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
        roundCaps_.put()));
  }

  ID2D1Factory* D2d() const { return d2d_.get(); }

  // The layout of `page` in a client area of `width` x `height` DIPs, the
  // page scrolled by `scroll` (clamped here).
  Layout Build(const UiContent& c, UiPage page, const std::string& profile, float width, float height, float& scroll,
               const LogState& logState) const {
    Layout l;
    // The log takes a wide window whole: its lines are long.
    const float pageWidth = std::min(page == UiPage::Logs ? width : kMaxPage, std::max(280.0f, width - 2 * kPad));
    const float x0 = std::max(kPad, (width - pageWidth) / 2);
    const float x1 = x0 + pageWidth;
    l.viewTop = kCaption;  // the page scrolls under our title bar
    l.viewBottom = height;
    Caption(l, c, width, page);  // fixed: before the page's items, which scroll
    const std::size_t first = l.items.size();

    float y = kCaption + 8;
    switch (page) {
      case UiPage::Overview: y = Overview(l, c, x0, x1, y, l.viewBottom - kPad); break;
      case UiPage::Servers: y = Servers(l, c, x0, x1, y); break;
      case UiPage::Subscription: y = Subscription(l, c, x0, x1, y); break;
      case UiPage::Logs: y = Logs(l, c, logState, x0, x1, y, height); break;
      case UiPage::Settings: y = Settings(l, c, x0, x1, y); break;
      case UiPage::Profile: y = ProfilePage(l, c, profile, x0, x1, y); break;
      case UiPage::Routing: y = Routing(l, c, x0, x1, y); break;
      case UiPage::Checks: y = Checks(l, c, x0, x1, y); break;
    }
    l.contentHeight = y + kPad - l.viewTop;  // from under our title bar, as the viewport

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
    if (l.glow) {
      l.glow->top -= scroll;
      l.glow->bottom -= scroll;
    }
    if (c.question) {
      // Over everything, the page's clip off: its card, then its buttons as
      // they stand, left to right (Tab's order).
      const UiQuestion& q = *c.question;
      const AskBoxes box = AskBox(q, width, height);
      Item card = Make(Kind::Question, box.card, q.title);
      card.index = static_cast<int>(q.tone);
      card.scrolls = false;
      l.items.push_back(std::move(card));
      Item yes = Make(q.tone == UiQuestion::Tone::Danger ? Kind::DangerButton : Kind::AccentButton, box.yes, q.yes);
      yes.action = ItemAction::Answer;
      yes.index = 1;
      yes.sub = static_cast<int>(q.tone);
      yes.scrolls = false;
      l.items.push_back(std::move(yes));
      if (!q.other.empty()) {
        Item other = Make(Kind::Button, box.other, q.other);
        other.action = ItemAction::Answer;
        other.index = 2;
        other.scrolls = false;
        l.items.push_back(std::move(other));
      }
      if (!q.no.empty()) {
        Item no = Make(Kind::Button, box.no, q.no);
        no.action = ItemAction::Answer;
        no.scrolls = false;
        l.items.push_back(std::move(no));
      }
      if (q.calendar) {
        // The calendar's keys are answers too (kAnswerMonth.. and a day's
        // kAnswerDay + number): they keep the card open, Answer moves the date.
        const UiCalendar& cal = *q.calendar;
        const CalendarKeys keys = CalendarHead(box.calHead);
        const auto step = [&](D2D1_RECT_F rect, int index, int direction) {
          Item it = Make(Kind::Day, rect, {}, kGlyphChevron);
          it.action = ItemAction::Answer;
          it.index = index;
          it.sub = direction;
          it.scrolls = false;
          l.items.push_back(std::move(it));
        };
        step(keys.monthBack, kAnswerMonthBack, -1);
        step(keys.monthOn, kAnswerMonthOn, 1);
        step(keys.yearBack, kAnswerYearBack, -1);
        step(keys.yearOn, kAnswerYearOn, 1);
        const int firstWeekday = WeekdayOf(cal.year, cal.month, 1);
        for (int day = 1; day <= DaysInMonth(cal.year, cal.month); ++day) {
          Item it = Make(Kind::Day, CalendarDay(box.calGrid, firstWeekday, day), std::to_wstring(day));
          it.action = ItemAction::Answer;
          it.index = kAnswerDay + day;
          it.checked = day == cal.day;
          it.sub = cal.year == cal.todayYear && cal.month == cal.todayMonth && day == cal.todayDay ? 1 : 0;
          it.scrolls = false;
          l.items.push_back(std::move(it));
        }
      }
    }
    return l;
  }

  // Where MainWindow::Ask's card and what's on it go in a window of
  // width x height: a little above the middle, a glyph in a circle, the
  // title and what it means beside it, the buttons on a band at the bottom -
  // yes first, as Windows' own dialogs have it.
  struct AskBoxes {
    D2D1_RECT_F card{};
    D2D1_RECT_F circle{};
    D2D1_RECT_F title{};
    D2D1_RECT_F text{};
    float band = 0;  // the buttons' band's top
    D2D1_RECT_F yes{};
    D2D1_RECT_F other{};
    D2D1_RECT_F no{};
    D2D1_RECT_F calHead{};  // a calendar's month and year keys; the weekdays' row and the days' grid under it
    D2D1_RECT_F calWeek{};
    D2D1_RECT_F calGrid{};
  };
  // A calendar's parts in the card's inner width (AskBoxes::calHead): the
  // month's name between its two steps on the left, the year's on the right.
  static constexpr float kCalCell = 26;  // a day's row
  static constexpr int kCalRows = 6;
  struct CalendarKeys {
    D2D1_RECT_F monthBack, monthName, monthOn, yearBack, yearName, yearOn;
  };
  static CalendarKeys CalendarHead(D2D1_RECT_F head) {
    const float step = 28;
    const float month = std::round((head.right - head.left - 8) * 0.56f);  // 8: the gap between the groups
    const float yearLeft = head.left + month + 8;
    return {{head.left, head.top, head.left + step, head.bottom},
            {head.left + step, head.top, head.left + month - step, head.bottom},
            {head.left + month - step, head.top, head.left + month, head.bottom},
            {yearLeft, head.top, yearLeft + step, head.bottom},
            {yearLeft + step, head.top, head.right - step, head.bottom},
            {head.right - step, head.top, head.right, head.bottom}};
  }
  // The cell of day `day` of a month starting on `firstWeekday` (0: Monday).
  static D2D1_RECT_F CalendarDay(D2D1_RECT_F grid, int firstWeekday, int day) {
    const int slot = firstWeekday + day - 1;
    const float width = (grid.right - grid.left) / 7;
    const int column = slot % 7;
    const int row = slot / 7;
    const float left = grid.left + width * static_cast<float>(column);
    const float top = grid.top + kCalCell * static_cast<float>(row);
    return {left + 3, top + 1, left + width - 3, top + kCalCell - 1};
  }
  AskBoxes AskBox(const UiQuestion& q, float width, float height) const {
    constexpr float kAskPad = 22;
    constexpr float kCircle = 38;
    constexpr float kBand = 68;
    constexpr float kAskButton = 34;
    const float w = std::min(392.0f, width - 2 * kPad);
    const float textLeft = kAskPad + kCircle + 14;
    const float textWidth = w - textLeft - kAskPad;
    const float titleHeight = TextHeight(q.title, ask_.get(), textWidth);
    const float textHeight = q.text.empty() ? 0.0f : TextHeight(q.text, wrap_.get(), textWidth);
    // The title's first line centred on the circle.
    const float titleTop = kAskPad + (kCircle - 24) / 2;
    const float body = std::max(kAskPad + kCircle, titleTop + titleHeight + (q.text.empty() ? 0.0f : 6 + textHeight));
    // With a second way on, the buttons go in two rows: yes across the card,
    // then the other and no side by side.
    // A calendar goes under it all, and with it the three buttons stand in one
    // row (the card has to fit the window's least height).
    const bool rows = !q.other.empty() && !q.calendar;
    const float calendar = q.calendar ? 14 + 30 + 22 + kCalCell * kCalRows : 0.0f;
    const float h = body + calendar + kAskPad + kBand + (rows ? kAskButton + 8 : 0.0f);
    const float left = std::round((width - w) / 2);
    const float top = std::round(std::max(kCaption + 8, (height - h) * 0.42f));
    AskBoxes b;
    b.card = {left, top, left + w, top + h};
    b.circle = {left + kAskPad, top + kAskPad, left + kAskPad + kCircle, top + kAskPad + kCircle};
    b.title = {left + textLeft, top + titleTop, left + textLeft + textWidth, top + titleTop + titleHeight};
    b.text = {b.title.left, b.title.bottom + 6, b.title.right, b.title.bottom + 6 + textHeight};
    if (q.calendar) {
      const float inner = left + kAskPad;
      const float headTop = top + body + 14;
      b.calHead = {inner, headTop, left + w - kAskPad, headTop + 30};
      b.calWeek = {inner, headTop + 30, left + w - kAskPad, headTop + 52};
      b.calGrid = {inner, headTop + 52, left + w - kAskPad, headTop + 52 + kCalCell * kCalRows};
    }
    b.band = top + body + calendar + kAskPad;
    const float buttonTop = b.band + (kBand - kAskButton) / 2;
    const float half = (w - 2 * kAskPad - 8) / 2;
    // Alone (a note), its one button takes the right half.
    b.yes = q.no.empty() ? D2D1_RECT_F{left + w - kAskPad - half, buttonTop, left + w - kAskPad, buttonTop + kAskButton}
                         : D2D1_RECT_F{left + kAskPad, buttonTop, left + kAskPad + half, buttonTop + kAskButton};
    b.no = {left + w - kAskPad - half, buttonTop, left + w - kAskPad, buttonTop + kAskButton};
    if (rows) {
      const float lower = buttonTop + kAskButton + 8;
      b.yes = {left + kAskPad, buttonTop, left + w - kAskPad, buttonTop + kAskButton};
      b.other = {left + kAskPad, lower, left + kAskPad + half, lower + kAskButton};
      b.no = {left + w - kAskPad - half, lower, left + w - kAskPad, lower + kAskButton};
    }
    if (q.calendar && !q.other.empty()) {
      const float third = (w - 2 * kAskPad - 16) / 3;
      const float x = left + kAskPad;
      b.yes = {x, buttonTop, x + third, buttonTop + kAskButton};
      b.other = {x + third + 8, buttonTop, x + 2 * third + 8, buttonTop + kAskButton};
      b.no = {x + 2 * third + 16, buttonTop, left + w - kAskPad, buttonTop + kAskButton};
    }
    return b;
  }

  // Draws a layout.
  void Draw(ID2D1RenderTarget* t, const Layout& l, const UiContent& c, const Interaction& in, const LogView& log,
            const LiveTraffic* live = nullptr) const {
    wil::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(t->CreateSolidColorBrush(Rgb(0, 0, 0), brush.put()))) {
      return;
    }
    Canvas k{t, brush.get(), dwrite_.get(), Flags(t), in.cube ? &*in.cube : nullptr};
    const D2D1_SIZE_F size = t->GetSize();
    t->Clear(FromColorRef(ui::kWindowColor));
    if (l.glow) {
      DrawBackdrop(t, size, *l.glow, c.display == Display::On);
      if (live != nullptr) {
        DrawLive(k, *live, *l.glow);  // under everything else: behind the glass, under the cards
      }
    }

    bool clipped = false;
    // Fixed items after the page's (the switch standing up over the
    // navigation): drawn once the page's clip is off, over everything.
    std::vector<std::size_t> over;
    // A question coming in (questionShown): its card and buttons grow from
    // a little smaller round the card's middle, fading in.
    std::optional<D2D1_POINT_2F> askAt;
    for (const Item& it : l.items) {
      if (it.kind == Kind::Question) {
        askAt = D2D1::Point2F((it.rect.left + it.rect.right) / 2, (it.rect.top + it.rect.bottom) / 2);
      }
    }
    const float shown = EaseOut(std::clamp(questionShown, 0.0f, 1.0f));
    const auto draw = [&](std::size_t i) {
      const Item& it = l.items[i];
      const auto n = static_cast<int>(i);
      if (it.kind == Kind::Question) {
        k.Fill({0, 0, size.width, size.height}, Rgb(0, 0, 0, 0.58f * shown));  // the page dimmed under it
      }
      wil::com_ptr<ID2D1Layer> layer;
      D2D1_MATRIX_3X2_F was{};
      if (askAt && shown < 1 && (it.kind == Kind::Question || it.action == ItemAction::Answer) &&
          SUCCEEDED(t->CreateLayer(nullptr, layer.put()))) {
        const float scale = 0.95f + 0.05f * shown;
        t->GetTransform(&was);
        t->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, *askAt) * was);
        t->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                           D2D1::IdentityMatrix(), shown),
                     layer.get());
      }
      if (it.clip >= 0) {  // a row of a fold that is moving: cut at its foot
        t->PushAxisAlignedClip({0, l.viewTop, size.width, std::max(l.viewTop, it.clip)}, D2D1_ANTIALIAS_MODE_ALIASED);
      }
      if (it.kind == Kind::LogBox && l.logBox) {
        DrawLog(k, *l.logBox, log);
      } else {
        DrawItem(k, it, c, n == in.hover, n == in.pressed);
      }
      if (it.clip >= 0) {
        t->PopAxisAlignedClip();
      }
      if (n == in.focus && in.focusVisible) {
        // A title bar key's ring goes round its pill: the cell runs to the window's edges.
        const float radius = it.chrome ? 10 : kRadius;
        t->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(it.chrome ? KeyPill(it) : it.rect, 2), radius, radius),
                                SetColor(brush.get(), Rgb(255, 255, 255, 0.9f)), 2);
      }
      if (layer) {
        t->PopLayer();
        t->SetTransform(was);
      }
    };
    for (std::size_t i = 0; i < l.items.size(); ++i) {
      const Item& it = l.items[i];
      if (!it.scrolls && clipped) {
        over.push_back(i);
        continue;
      }
      if (it.scrolls && !clipped) {
        t->PushAxisAlignedClip({0, l.viewTop, size.width, l.viewBottom}, D2D1_ANTIALIAS_MODE_ALIASED);
        clipped = true;
      }
      if (it.scrolls && (it.rect.bottom < l.viewTop || it.rect.top > l.viewBottom)) {
        continue;
      }
      draw(i);
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
    for (const std::size_t i : over) {
      draw(i);
    }
    if (in.tip >= 0 && static_cast<std::size_t>(in.tip) < l.items.size()) {
      DrawTip(k, l.items[static_cast<std::size_t>(in.tip)], size);
    }
  }

  IWICImagingFactory* Wic() const { return wic_.get(); }

  // How wide a line of body text is: for what the window lays over the page.
  float BodyWidth(std::wstring_view text) const { return TextWidth(text, body_.get()); }

  // A layout as text, an item a line - its kind, rectangle, text - with "!"
  // where a line of text needs more room than the item gives it (it's cut
  // with "…" then), and "x" where two clickable items overlap other than a
  // "?" or a button over its row, ">" where it leaves the window, and "e"
  // where its edge from the card that holds it is off the law's three lines
  // (OnTheLines). For tests/ui: the layout checked without looking at pictures.
  //
  // docs/design/law.md, Grid Logic: from a card's edge a row's zone lies 4
  // off it, a plate (a segment) 8, the content - text, things, dividers - 20,
  // and a key, whose zone reaches 6 past its ink, 14. `left` and `right` are
  // the distances from the card's edges. What flows (text, things, a count)
  // may lie anywhere inside the content's lines.
  static bool OnTheLines(const Item& it, float left, float right) {
    const auto at = [](float v, float line) { return std::abs(v - line) <= 0.5f; };
    switch (it.kind) {
      case Kind::Switch:
      case Kind::Choice:
      case Kind::ValueRow:
      case Kind::CheckRow:
      case Kind::AppRow:
      case Kind::ServerCard: return at(left, 4) && at(right, 4);
      case Kind::ProfileRow: return at(left, 4) && right >= 4 - 0.5f;  // cut short by its switch
      case Kind::Toggle: return at(right, 8);  // a plate 64 wide round a switch 40: the switch's ink on 20
      case Kind::Divider: return at(left, 20) && at(right, 20);
      case Kind::Segment: return at(left, 8) || at(right, 8);  // the outer edge on the line 8, the inner 4 off the middle
      case Kind::IconButton: {  // the zone reaches past the glyph, which keeps off the line 20
        const float bleed = (it.rect.right - it.rect.left - 16) / 2;
        return std::min(left, right) + bleed >= 20 - 0.5f;
      }
      case Kind::Banner:
      case Kind::SpeedGraph:
      case Kind::LogBox: return true;  // not on a card's lines: a card of its own, or the page's
      default: return left >= 20 - 0.5f && right >= 20 - 0.5f;
    }
  }
  static std::wstring OneLine(std::wstring_view text, std::size_t max) {
    std::wstring line(text.substr(0, max));
    std::replace(line.begin(), line.end(), L'\n', L' ');
    return line;
  }
  std::wstring Describe(const Layout& l, float width) const {
    static constexpr std::array<const wchar_t*, 35> kNames = {
        L"Card",     L"Divider",  L"Title",    L"Heading",  L"Text",     L"Muted",  L"Wrap",   L"Caption",
        L"Error",    L"Field",    L"Power",    L"Server",   L"Banner",
        L"Button",   L"Accent",   L"Danger",   L"Icon",     L"Switch",   L"Choice", L"Profile", L"Toggle",
        L"Check",    L"Segment",  L"Chip",     L"App",      L"Log",      L"Hint",   L"Value",   L"Meter",  L"Tag",
        L"AppTitle", L"Speed",    L"Question", L"Pill",    L"Day"};
    // One per Kind, in its order: a kind added or gone shifts every name after it.
    static_assert(kNames.size() == static_cast<std::size_t>(Kind::Day) + 1);
    std::wstring out;
    for (std::size_t i = 0; i < l.items.size(); ++i) {
      const Item& it = l.items[i];
      const D2D1_RECT_F r = Seen(it);
      const float w = r.right - r.left;
      // Room for the text on the item's line, as DrawItem gives it.
      float room = 0;
      float need = 0;
      switch (it.kind) {
        case Kind::Title: room = w; need = TextWidth(it.text, title_.get()); break;
        case Kind::Heading: room = w; need = TextWidth(it.text, heading_.get()); break;
        case Kind::Button:
        case Kind::AccentButton:
        case Kind::DangerButton:
          room = w - 4;
          need = TextWidth(it.text, body_.get()) + (it.glyph.empty() ? 0.0f : 26.0f);
          break;
        case Kind::Switch: room = w - 16 - 76; need = TextWidth(it.text, body_.get()); break;
        case Kind::Choice: room = w - 42 - 120; need = TextWidth(it.text, body_.get()); break;
        case Kind::ProfileRow: room = w - 14 - 28; need = TextWidth(it.text, body_.get()); break;
        case Kind::ServerCard: room = w - 16 - 34; need = TextWidth(it.text, heading_.get()); break;
        case Kind::ValueRow:
          room = w - 16 - 30;
          need = TextWidth(it.text, body_.get()) + 12 + TextWidth(it.detail, trailing_.get()) +
                 (it.logo.empty() ? 0.0f : 22.0f) +
                 (it.hinted ? kHint + 6 : 0.0f);
          break;
        case Kind::Segment: room = w - 8; need = TextWidth(it.text, button_.get()); break;
        case Kind::CheckRow:
          room = w - 34 - CheckButtonRoom(it);
          need = TextWidth(it.text, body_.get()) + 6 + kHint + 8 + TextWidth(it.detail, trailing_.get());
          break;
        case Kind::Muted:  // a line of its own; a taller one wraps
          if (it.rect.bottom - it.rect.top <= 24 && it.text.find(L'\n') == std::wstring::npos) {
            room = w;
            need = TextWidth(it.text, caption_.get());
          }
          break;
        default: break;
      }
      bool overlaps = false;
      if (Interactive(it) && it.kind != Kind::Hint) {
        for (std::size_t j = i + 1; j < l.items.size(); ++j) {
          const Item& o = l.items[j];
          const D2D1_RECT_F s = Seen(o);
          const bool over = Interactive(o) && s.left < r.right && s.right > r.left && s.top < r.bottom && s.bottom > r.top;
          // A card that is a link holds buttons and lines that are links of their own: they take the click.
          // A question's buttons lie over the page, which takes no click while it's open.
          const bool allowed = o.kind == Kind::Hint || o.action == ItemAction::Answer ||
                               (o.kind == Kind::IconButton && (it.kind == Kind::Title || it.kind == Kind::Card)) ||
                               ((o.kind == Kind::Muted || o.kind == Kind::Button) && it.kind == Kind::Card);
          overlaps = overlaps || (over && !allowed);
        }
      }
      const bool outside = r.left < 0 || r.right > width + 0.5f;
      // The card that holds it: the nearest one before it that has it inside. A question's buttons and the
      // calendar's days lie over the page, not on a card.
      bool off = false;
      if (it.kind != Kind::Card && it.kind != Kind::Question && it.kind != Kind::Day && it.action != ItemAction::Answer) {
        for (std::size_t j = i; j-- > 0;) {
          const D2D1_RECT_F c = l.items[j].rect;
          if (l.items[j].kind != Kind::Card || c.left > r.left + 0.5f || c.right < r.right - 0.5f ||
              c.top > r.top + 0.5f || c.bottom < r.bottom - 0.5f) {
            continue;
          }
          off = !OnTheLines(it, r.left - c.left, c.right - r.right);
          if (it.kind == Kind::Tag) {  // things flow: the first of a row on the line 20, the rest 4 after the one before
            const Item* before = nullptr;
            for (std::size_t k = i; k-- > j + 1 && before == nullptr;) {
              before = l.items[k].kind == Kind::Tag ? &l.items[k] : nullptr;
            }
            const bool sameRow = before != nullptr && std::abs(before->rect.top - r.top) <= 0.5f;
            const bool first = std::abs(r.left - c.left - 20) <= 0.5f;
            off = off || (sameRow ? std::abs(before->rect.right + 4 - r.left) > 0.5f
                                  : !first || (before != nullptr && std::abs(before->rect.bottom + 4 - r.top) > 0.5f));
          }
          break;
        }
      }
      out += std::format(L"{}{}{}{} {:<12} [{:6.1f} {:6.1f} {:6.1f} {:6.1f}] {}", need > room + 0.5f ? L"!" : L" ",
                         overlaps ? L"x" : L" ", outside ? L">" : L" ", off ? L"e" : L" ",
                         kNames[static_cast<std::size_t>(it.kind)], r.left, r.top, r.right, r.bottom, OneLine(it.text, 60));
      if (need > room + 0.5f) {
        out += std::format(L"   (needs {:.0f}, has {:.0f})", need, room);
      }
      if (!it.detail.empty() && it.kind != Kind::AppRow) {
        out += L"  | " + OneLine(it.detail, 50);
      }
      out += L"\n";
    }
    return out;
  }

  // The program icons and the flags are the render target's: a new target
  // needs new ones.
  void ForgetIcons() const {
    icons_.clear();
    logos_.clear();
    flags_.reset();
    flagsLoaded_ = false;
  }

 private:
  // Geist, in the exe (a resource); Segoe UI if it could not be loaded (a
  // test's exe has none).
  const wchar_t* Display() const { return fonts_ ? L"Geist" : L"Segoe UI Variable Display"; }
  const wchar_t* Text() const { return fonts_ ? L"Geist" : L"Segoe UI Variable Text"; }

  // The fonts of the exe's resources as a collection; null if any is missing.
  static wil::com_ptr<IDWriteFontCollection> LoadFontCollection(IDWriteFactory* factory) {
    wil::com_ptr<IDWriteFactory5> factory5;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(factory5.put())))) {
      return nullptr;
    }
    // The loader must outlive every format that uses the collection.
    static wil::com_ptr<IDWriteInMemoryFontFileLoader> loader;
    if (!loader) {
      if (FAILED(factory5->CreateInMemoryFontFileLoader(loader.put())) ||
          FAILED(factory5->RegisterFontFileLoader(loader.get()))) {
        loader.reset();
        return nullptr;
      }
    }
    wil::com_ptr<IDWriteFontSetBuilder1> builder;
    if (FAILED(factory5->CreateFontSetBuilder(builder.put()))) {
      return nullptr;
    }
    for (const int id : {IDR_FONT_TEXT}) {
      const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
      const HGLOBAL loaded = found != nullptr ? LoadResource(nullptr, found) : nullptr;
      const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
      wil::com_ptr<IDWriteFontFile> file;
      if (data == nullptr ||
          FAILED(loader->CreateInMemoryFontFileReference(factory5.get(), data, SizeofResource(nullptr, found), nullptr,
                                                          file.put())) ||
          FAILED(builder->AddFontFile(file.get()))) {
        return nullptr;
      }
    }
    wil::com_ptr<IDWriteFontSet> set;
    wil::com_ptr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(set.put())) ||
        FAILED(factory5->CreateFontCollectionFromFontSet(set.get(), collection.put()))) {
      return nullptr;
    }
    return collection.query<IDWriteFontCollection>();
  }

  // Drawing helpers over one reusable brush.
  struct Canvas {
    ID2D1RenderTarget* t;
    ID2D1SolidColorBrush* b;
    IDWriteFactory* dwrite;  // for text with flags in it
    ID2D1Bitmap* sprite;     // the flags (flags.h); null if it couldn't be had
    const CubePose* cube;    // the switch's pose mid-motion; null: its state's (cube.h)

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
    // Metal under a light from above: a face of `r`, a bright 1 px edge along
    // its top, a dark one along its bottom, a hairline round it. Pressed, the
    // light is gone and the face sinks (a dark edge on top instead).
    void Bevel(D2D1_RECT_F r, float radius, float face, bool pressed) const {
      Round(r, radius, Rgb(255, 255, 255, pressed ? face * 0.5f : face));
      Outline(r, radius, Rgb(255, 255, 255, 0.07f));
      const float inset = radius * 0.8f;
      if (pressed) {
        Line({r.left + inset, r.top + 1.5f}, {r.right - inset, r.top + 1.5f}, Rgb(0, 0, 0, 0.40f));
      } else {
        Line({r.left + inset, r.top + 1.5f}, {r.right - inset, r.top + 1.5f}, Rgb(255, 255, 255, 0.11f));
        Line({r.left + inset, r.bottom - 1.5f}, {r.right - inset, r.bottom - 1.5f}, Rgb(0, 0, 0, 0.32f));
      }
    }
    // A pilot lamp: a point of `c` in a halo that thins out.
    void Led(D2D1_POINT_2F at, float radius, D2D1_COLOR_F c) const {
      for (const auto [grow, alpha] : {std::pair{3.0f, 0.07f}, std::pair{2.0f, 0.12f}, std::pair{1.0f, 0.22f}}) {
        t->FillEllipse(D2D1::Ellipse(at, radius + grow, radius + grow), Color({c.r, c.g, c.b, c.a * alpha}));
      }
      t->FillEllipse(D2D1::Ellipse(at, radius, radius), Color(c));
    }
    void Text(std::wstring_view s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) const {
      if (s.empty()) {
        return;
      }
      if (sprite == nullptr || FindFlagEmoji(s).empty()) {
        t->DrawText(s.data(), static_cast<UINT32>(s.size()), f, r, Color(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
      }
      std::vector<FlagInline> flags;
      if (const auto layout = MakeTextLayout(dwrite, s, f, r.right - r.left, r.bottom - r.top, flags, t, sprite)) {
        t->DrawTextLayout({r.left, r.top}, layout.get(), Color(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
    THROW_IF_FAILED(dwrite_->CreateTextFormat(family, fonts_ && wcscmp(family, L"Geist") == 0 ? fonts_.get() : nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
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

  // This exe's path: the app's icon is taken from it (Kind::AppTitle).
  static const std::wstring& SelfPath() {
    static const std::wstring path = [] {
      std::wstring buffer(MAX_PATH, L'\0');
      const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
      buffer.resize(n > 0 && n < buffer.size() ? n : 0);
      return buffer;
    }();
    return path;
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

  // A service's logo (icons.h ServiceLogo), 16 DIPs at the target's DPI;
  // null if it has none. Drawn once per id (a miss too).
  ID2D1Bitmap* Logo(ID2D1RenderTarget* target, const std::string& id) const {
    const auto [it, fresh] = logos_.try_emplace(id);
    if (!fresh) {
      return it->second.get();
    }
    FLOAT dpiX = 96;
    FLOAT dpiY = 96;
    target->GetDpi(&dpiX, &dpiY);
    const auto size = static_cast<int>(std::lround(16 * dpiX / 96.0f));
    const std::vector<std::uint32_t> pixels = ServiceLogo(id, size);
    if (!pixels.empty()) {
      const auto side = static_cast<UINT32>(size);
      target->CreateBitmap({side, side}, pixels.data(), side * 4,
                           D2D1::BitmapProperties(
                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX, dpiY),
                           it->second.put());
    }
    return it->second.get();
  }

  // The flags sprite (flags.h) from the exe's resources, as a bitmap of
  // `target` at 96 DPI - so a flag's source rectangle is its pixels. Null if
  // it can't be had; tried once per target.
  ID2D1Bitmap* Flags(ID2D1RenderTarget* target) const {
    if (!flagsLoaded_) {
      flagsLoaded_ = true;
      flags_ = ResourceBitmap(target, IDR_FLAGS);
    }
    return flags_.get();
  }

  // A picture in the exe's resources (RCDATA: PNG or JPEG) as a bitmap of
  // `target` at 96 DPI - its pixels are DIPs. Null if it can't be had.
  wil::com_ptr<ID2D1Bitmap> ResourceBitmap(ID2D1RenderTarget* target, int id) const {
    wil::com_ptr<ID2D1Bitmap> bitmap;
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL data = resource != nullptr ? LoadResource(module, resource) : nullptr;
    void* bytes = data != nullptr ? LockResource(data) : nullptr;
    if (bytes == nullptr || !wic_) {
      return bitmap;
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
      target->CreateBitmapFromWicBitmap(converter.get(), &props, bitmap.put());
    }
    return bitmap;
  }

  // The height `text` takes wrapped to `width` in `format`.
  float TextHeight(std::wstring_view text, IDWriteTextFormat* format, float width) const {
    const auto metrics = Metrics(text, format, width, 10000);
    return metrics ? std::ceil(metrics->height) : 20;
  }

  // What `text` takes in `format` within width x height, laid out as
  // Canvas::Text lays it out - flags and all. Nullopt if DirectWrite fails.
  std::optional<DWRITE_TEXT_METRICS> Metrics(std::wstring_view text, IDWriteTextFormat* format, float width,
                                             float height) const {
    std::vector<FlagInline> flags;
    const auto layout = MakeTextLayout(dwrite_.get(), text, format, width, height, flags);
    DWRITE_TEXT_METRICS metrics{};
    if (!layout || FAILED(layout->GetMetrics(&metrics))) {
      return std::nullopt;
    }
    return metrics;
  }

  /* ---- layout ---- */

  // A page's top: back to the overview, the title, and a wrapped subtitle.
  // How wide `text` is on one line in `format`.
  float TextWidth(std::wstring_view text, IDWriteTextFormat* format) const {
    const auto m = Metrics(text, format, 10000, 100);
    return m ? m->widthIncludingTrailingWhitespace : 0.0f;
  }

  // A "?" at `x`, centered on `cy`: pointed at, `text` shows in a tip.
  static void AddHint(Layout& l, float x, float cy, std::wstring text, bool scrolls = true) {
    Item hint = Make(Kind::Hint, {x, cy - kHint / 2, x + kHint, cy + kHint / 2}, std::move(text));
    hint.action = ItemAction::Hint;
    hint.scrolls = scrolls;
    l.items.push_back(std::move(hint));
  }

  // A "?" right after `text` drawn in `format` from `left` (as far as `right`
  // lets it), on the line centered on `cy`.
  void HintAfter(Layout& l, std::wstring_view text, IDWriteTextFormat* format, float left, float right, float cy,
                 std::wstring hint) const {
    const float x = std::min(left + TextWidth(text, format) + 6, right - kHint);
    AddHint(l, x, cy, std::move(hint));
  }

  // A page's top: back, the title (a "?" after it with `hint`), and under it
  // `subtitle` - what the page says now, not what it's for.
  float PageTitle(Layout& l, const wchar_t* title, const wchar_t* subtitle, float x0, float x1, float y,
                  UiPage backTo = UiPage::Overview, std::wstring hint = {}, float titleRight = 0) const {
    Item back = Make(Kind::IconButton, {x0 - 6, y, x0 + 30, y + 36}, L"Назад", kGlyphBack);
    back.action = ItemAction::Page;
    back.index = static_cast<int>(backTo);
    l.items.push_back(std::move(back));
    const float right = titleRight > 0 ? titleRight : x1;
    l.items.push_back(Make(Kind::Title, {x0 + 38, y, right - (hint.empty() ? 0 : kHint + 8), y + 36}, title));
    if (!hint.empty()) {
      HintAfter(l, title, title_.get(), x0 + 38, right, y + 18, std::move(hint));
    }
    y += 36 + 8;
    if (subtitle != nullptr && *subtitle != L'\0') {
      const float h = TextHeight(subtitle, captionWrap_.get(), x1 - x0);
      l.items.push_back(Make(Kind::Caption, {x0, y - 4, x1, y - 4 + h}, subtitle));
      y += h + 2;
    }
    return y + 8;
  }

  // A card's heading, a "?" after it with `hint`.
  float Heading(Layout& l, const std::wstring& text, float x0, float x1, float y, std::wstring hint = {}) const {
    l.items.push_back(Make(Kind::Heading, {x0, y, x1, y + 28}, text));
    if (!hint.empty()) {
      HintAfter(l, text, heading_.get(), x0, x1, y + 14, std::move(hint));
    }
    return y + 28 + 6;
  }

  // A card of switches, one row each; one with a hint has a "?" after its
  // title and no line under it, unless it has a detail too.
  struct SwitchRow {
    std::wstring title;
    std::wstring detail;  // a state worth a line ("при обрыве..."); empty: the title alone
    std::wstring hint;
    bool on = false;
    UiCommand command = UiCommand::Toggle;
    int index = 0;
    bool enabled = true;
  };
  static SwitchRow Row(std::wstring title, std::wstring hint, bool on, UiCommand command, std::wstring detail = {},
                       int index = 0, bool enabled = true) {
    SwitchRow row;
    row.title = std::move(title);
    row.hint = std::move(hint);
    row.on = on;
    row.command = command;
    row.detail = std::move(detail);
    row.index = index;
    row.enabled = enabled;
    return row;
  }
  float Switches(Layout& l, const std::vector<SwitchRow>& rows, float x0, float x1, float y) const {
    float h = 0;
    for (const SwitchRow& row : rows) {
      h += row.detail.empty() ? kSwitchRow : 60;
    }
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
    float top = y;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const SwitchRow& row = rows[i];
      const float rowHeight = row.detail.empty() ? kSwitchRow : 60;
      if (i > 0) {
        l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
      }
      Item item = CommandItem(Kind::Switch, {x0 + 4, top + 2, x1 - 4, top + rowHeight - 2}, row.title, nullptr,
                              row.command, row.index);
      item.detail = row.detail;
      item.checked = row.on;
      item.enabled = row.enabled;
      l.items.push_back(std::move(item));
      if (!row.hint.empty()) {
        // Where Kind::Switch draws the title: 16 in, on the row's middle or its top line.
        const float cy = row.detail.empty() ? top + rowHeight / 2 : top + 2 + 10 + 12;
        HintAfter(l, row.title, body_.get(), x0 + 20, x1 - 80, cy, row.hint);
      }
      top += rowHeight;
    }
    return y + h;
  }

  // The board: a bucket a way - through the proxy, directly, through WARP,
  // blocked - with what goes there in it: services, sites, programs and the
  // Russian sites, wrapped in rows. The bucket everything
  // else goes to (the mode, Routing's switch) doesn't show what changes
  // nothing there (UiChip::idle) and isn't there at all with nothing else in
  // it - the others are the mode's exceptions; an empty one is its head alone. A click
  // on one: where it goes instead, or away (ChipMenu); the "+" in a bucket's
  // corner: what can go there, a site typed across the bucket (BoardAdd).
  // WARP's bucket says how WARP goes (over the VPN, or directly where that got
  // nothing through - its own call, no pick); its "?" says what WARP is. The
  // block is a situational one: with nothing in it, a quiet line of its own
  // under the board, not a card (30.10.2026, his: "оставил тут просто чтоб он
  // не мешался"); the ads and the cookie windows are the settings' switches.
  float Board(Layout& l, const UiRouting& r, float x0, float x1, float y) const {
    struct Bucket {
      int id;
      const wchar_t* name;
      std::wstring note;
      std::wstring hint;
      std::wstring idleNote;  // said by the way's quiet line while nothing is on it: what it is for
    };
    const std::wstring warpHint =
        L"WARP — бесплатный выход через Cloudflare. Выручает там, где сайт не пускает с адресов VPN-серверов "
        L"или режет контент (ChatGPT, Netflix): сайт видит адрес Cloudflare. Google его адреса не любит — "
        L"Gemini и поиск через WARP упрутся в капчу.\n\nWARP идёт поверх "
        L"твоего VPN — провайдер его не видит. Не проходит так — Sovereign сам пустит WARP напрямую.";
    // Where everything else goes: said on that bucket, with how many it doesn't show.
    const int restBucket = r.finalDirect ? kBucketDirect : kBucketProxy;
    const auto idle = std::count_if(r.board.begin(), r.board.end(), [](const UiChip& chip) { return chip.idle; });
    const std::wstring rest = L"и всё остальное";
    const std::wstring restHint = RestHint(idle);
    const std::array<Bucket, 4> buckets = {{
        {kBucketProxy, L"Через VPN", restBucket == kBucketProxy ? rest : std::wstring(),
         restBucket == kBucketProxy ? restHint : std::wstring(), std::wstring()},
        {kBucketDirect, L"Напрямую", restBucket == kBucketDirect ? rest : std::wstring(),
         restBucket == kBucketDirect ? restHint : std::wstring(), std::wstring()},
        {kBucketWarp, L"Через WARP", r.warpState, warpHint, std::wstring()},
        // (31.10.2026, his: "не понимаю, чё в блок можно кинуть": the line says what goes in, the
        // "?" gives the examples.)
        {kBucketBlock, L"Блок", std::wstring(),
         L"Сюда — то, чему нужно закрыть сеть совсем: сайт, который не хочешь открывать; программа, которой нельзя "
         L"выходить в интернет (например, назойливый апдейтер); сервис из списка. Оно не откроется ни через VPN, "
         L"ни напрямую. Пока тут пусто — блокировать нечего.\n\nРекламу и баннеры cookie включают в «+» тут же "
         L"или в настройках — списком их добавлять не надо.\n\nОсторожно с «Microsoft»: в этот список входят GitHub, Windows Update, OneDrive "
         L"и Office — они перестанут открываться. Выключателем слежки Windows это не служит.",
         L"отрезать сайт или программу"},
    }};
    // A way's card: a rail of its colour down the left edge, its head - name,
    // "?", how many things, "+" - and its things wrapped under it. Compact
    // since 30.10.2026 (his: "компактнее карточки"): a head of 34, things of 28.
    // On the law's lines (law.md, Grid Logic): words and things between the
    // lines 20, 4 between the things both ways, 8 between the cards; the key's
    // zone ends on the line 14 - its glyph on 20.
    constexpr float kHead = 34;
    constexpr float kChipGap = 4;
    constexpr float kAddKey = 28;
    constexpr float kWayGap = 8;
    const float left = x0 + 20;
    const float right = x1 - 20;
    const float keyRight = x1 - 14;
    // The exceptions first, the way everything else goes last: it closes the board, with what's
    // named there set quieter (31.10.2026, his: "видеть, чё напрямую").
    std::array<const Bucket*, 4> order{};
    {
      std::size_t n = 0;
      for (const Bucket& b : buckets) {
        order[n] = &b;
        n += b.id == restBucket ? 0 : 1;
      }
      for (const Bucket& b : buckets) {
        if (b.id == restBucket) {
          order[n++] = &b;
        }
      }
    }
    for (const Bucket* bucket : order) {
      const Bucket& b = *bucket;
      // Its things in rows from the card's left, wrapped at its right.
      std::vector<std::pair<std::size_t, D2D1_RECT_F>> placed;
      float cx = left;
      float cy = 0;  // from the first row's top
      for (std::size_t n = 0; n < r.board.size(); ++n) {
        const UiChip& chip = r.board[n];
        if (chip.bucket != b.id) {
          continue;  // (what goes here anyway is shown too, quieter: he wants to see what goes directly)
        }
        // Whole pixels up: a chip as wide as its text to the last float got its
        // last letters trimmed to "…" (brave.exe -> "brave.e…", 30.10.2026).
        float w = 24 + std::ceil(TextWidth(chip.text, body_.get())) + (chip.icon.empty() && chip.logo.empty() ? 0.0f : 24.0f) +
                  (chip.detail.empty() ? 0.0f : 8 + std::ceil(TextWidth(chip.detail, caption_.get())));
        if (w > right - left) {
          w = right - left;
        }
        if (cx > left && cx + w > right) {
          cx = left;
          cy += kTag + kChipGap;
        }
        placed.emplace_back(n, D2D1_RECT_F{cx, cy, cx + w, cy + kTag});
        cx += w + kChipGap;
      }
      if (b.id == restBucket && placed.empty()) {
        continue;  // where everything else goes, and nothing else: not an exception, not shown
      }
      const auto addKey = [&](float top, float height) {
        Item add = CommandItem(Kind::IconButton, {keyRight - kAddKey, top, keyRight, top + height}, L"Добавить", kGlyphAdd,
                               UiCommand::BoardAdd, b.id);
        add.tip = std::format(L"Добавить в «{}»: сервис, программу или сайт", b.name);
        return add;
      };
      if (placed.empty()) {
        // Nothing sent this way: a muted line of its own, its rail faint and
        // no card - it's there to be found, not to be looked at (the block's
        // way since 30.10.2026, his: "оставил тут просто чтоб он не
        // мешался"; every empty way's since).
        constexpr float kQuiet = 30;
        Item rail = Make(Kind::Card, {x0, y, x1, y + kQuiet});
        rail.rail = b.id;
        rail.quiet = true;
        l.items.push_back(std::move(rail));
        const float nameWidth = TextWidth(b.name, caption_.get());
        l.items.push_back(Make(Kind::Muted, {left, y, left + nameWidth + 4, y + kQuiet}, b.name));
        if (!b.hint.empty()) {
          HintAfter(l, b.name, caption_.get(), left, keyRight - kAddKey - 8, y + kQuiet / 2, b.hint);
        }
        // What the way is for, after the "?" - only while it fits whole (never cut to "…").
        if (!b.idleNote.empty()) {
          const float noteLeft = left + nameWidth + 6 + kHint + 8;
          const float noteRight = keyRight - kAddKey - 8;
          if (noteLeft + std::ceil(TextWidth(b.idleNote, caption_.get())) + 4 <= noteRight) {
            l.items.push_back(Make(Kind::Muted, {noteLeft, y, noteRight, y + kQuiet}, b.idleNote));
          }
        }
        l.items.push_back(addKey(y + 1, kQuiet - 2));
        y += kQuiet + kWayGap;
        continue;
      }
      // Its name the card's title (heading, 15.75): the section heading over the board is the group's.
      const float nameWidth = TextWidth(b.name, heading_.get());
      float noteLeft = left + nameWidth + 8;
      if (!b.hint.empty()) {
        noteLeft += kHint + 8;
      }
      // How many are on it, engraved in a pill by the "+".
      const std::wstring count = std::format(L"{:02}", placed.size());
      const float countWidth = TextWidth(count, caption_.get()) + 2 * kPillPad;
      const float countRight = keyRight - kAddKey - 8;
      // The note (WARP's "через «server»") by the name while it fits; else a line of its own under
      // the head, wrapped - a value is never cut to "…" (31.10.2026: "через «AnyTL…" at 350 wide).
      const float noteRoom = countRight - countWidth - 8 - noteLeft;
      const bool noteBelow = !b.note.empty() && std::ceil(TextWidth(b.note, caption_.get())) + 4 > noteRoom;
      const float noteHeight = noteBelow ? std::max(18.0f, TextHeight(b.note, captionWrap_.get(), right - left)) : 0.0f;
      const float head = kHead + noteHeight;
      const float h = head + cy + kTag + 8;
      Item card = Make(Kind::Card, {x0, y, x1, y + h});
      card.rail = b.id;
      l.items.push_back(std::move(card));
      l.items.push_back(Make(Kind::Heading, {left, y + 3, left + nameWidth + 4, y + kHead - 3}, b.name));
      if (!b.hint.empty()) {
        HintAfter(l, b.name, heading_.get(), left, countRight - countWidth - 8, y + kHead / 2, b.hint);
      }
      l.items.push_back(Make(Kind::Pill,
                             {countRight - countWidth, y + (kHead - kPillHeight) / 2, countRight, y + (kHead + kPillHeight) / 2},
                             count));
      if (noteBelow) {
        l.items.push_back(Make(Kind::Caption, {left, y + kHead - 6, right, y + kHead - 6 + noteHeight}, b.note));
      } else if (!b.note.empty()) {
        l.items.push_back(Make(Kind::Muted, {noteLeft, y + 3, countRight - countWidth - 8, y + kHead - 3}, b.note));
      }
      l.items.push_back(addKey(y + 3, kAddKey));
      for (const auto& [n, at] : placed) {
        const UiChip& chip = r.board[n];
        const D2D1_RECT_F place = {at.left, y + head + at.top, at.right, y + head + at.bottom};
        Item tag = CommandItem(Kind::Tag, place, chip.text, nullptr, UiCommand::ChipMenu, static_cast<int>(n));
        tag.tip = L"Перенести в другой путь или убрать";
        tag.detail = chip.detail;
        tag.text2 = chip.icon;
        tag.logo = chip.logo;
        tag.quiet = chip.idle;
        l.items.push_back(std::move(tag));
      }
      y += h + kWayGap;
    }
    return y;
  }

  // What the hidden things on the way everything else goes to say of
  // themselves (UiChip::idle); empty with none.
  static std::wstring RestHint(std::ptrdiff_t idle) {
    return idle > 0 ? std::wstring(L"Сюда идёт всё, что не названо выше. Названное здесь приглушено: оно шло бы сюда "
                                   L"и без записи, поэтому ничего не меняет. Это не исключения; если переключить режим "
                                   L"наверху, они ими станут.")
                    : std::wstring();
  }
  float Paragraph(Layout& l, Kind kind, const std::wstring& text, float x0, float x1, float y) const {
    const float indent = kind == Kind::ErrorText ? 28.0f : 0.0f;
    const float h =
        std::max(20.0f, TextHeight(text, kind == Kind::Caption ? captionWrap_.get() : wrap_.get(), x1 - x0 - indent));
    l.items.push_back(Make(kind, {x0, y, x1, y + h}, text, kind == Kind::ErrorText ? kGlyphWarning : nullptr));
    return y + h;
  }

  // Our own title bar (the system's is gone - Impl's WM_NCCALCSIZE): the
  // name on the left, the report and a new version by it; on the right the
  // cross, hiding the window to the tray, and where they belong: on the main
  // screen the routing, the log and the settings; on the configurations' list adding one (the
  // clipboard, a QR code, a file) - the main screen has it on its
  // subscription card. Inside the settings they were in the way (he asked
  // them gone, 16.10.2026). A new version found, its key on every page (it
  // was the main screen's banner till 24.10.2026). The rest of it drags the
  // window (WM_NCHITTEST).
  void Caption(Layout& l, const UiContent& c, float width, UiPage page) const {
    // Keys of one width, the cross's too (it was Windows' 46): their pills
    // (DrawChrome) a step apart, the cross's as far from the window's right
    // edge as from its top. Its cell still runs to the edge - a throw of the
    // mouse into the corner finds it.
    float right = width - 40;  // the next key's right edge, leftwards from the cross
    const auto chrome = [](Item item) {
      item.scrolls = false;
      item.chrome = true;
      return item;
    };
    // The cross closes the client (the connection stays: the service holds
    // it); the dash beside it only puts the window away to the tray.
    Item close = chrome(CommandItem(Kind::IconButton, {right, 0, width, kCaption}, L"Закрыть клиент (подключение останется)",
                                 kGlyphClose, UiCommand::Exit));
    close.tip = L"Закрыть клиент. VPN останется: его держит служба";
    Item hide = chrome(Make(Kind::IconButton, {right - 40, 0, right, kCaption}, L"Свернуть в трей", kGlyphMinimize));
    hide.action = ItemAction::Hide;
    hide.tip = L"Убрать окно в трей. VPN работает дальше";
    right -= 40;
    // The keys between them a step closer (36): on the main screen five of
    // them, the name, the report and a new version's pill fit into 360.
    constexpr float kKey = 36;
    std::vector<Item> keys;
    const auto page_key = [&](const wchar_t* text, const wchar_t* glyph, UiPage to, const wchar_t* tip) {
      Item key = chrome(Make(Kind::IconButton, {right - kKey, 0, right, kCaption}, text, glyph));
      key.action = ItemAction::Page;
      key.index = static_cast<int>(to);
      key.tip = tip;
      keys.push_back(std::move(key));
      right -= kKey;
    };
    // On the main screen what is opened now and then a click away (the
    // wrench's menu of 17.10 gone, 30.10.2026, his: "журнал и маршруты рядом
    // с шестиренкой"); the checks are a row of the settings.
    if (page == UiPage::Overview) {
      page_key(L"Настройки", kGlyphSettings, UiPage::Settings, L"Настройки: Kill switch, DNS, автозапуск, проверка");
      page_key(L"Журнал", kGlyphLog, UiPage::Logs, L"Журнал: что делает ядро, ошибки");
      page_key(L"Маршруты", kGlyphRoute, UiPage::Routing, L"Маршруты: что идёт через VPN, а что напрямую");
    }
    if (page == UiPage::Subscription) {
      // `keys` is read backwards, so the last way goes first: they stand paste, QR, file.
      for (auto way = kAddWays.rbegin(); way != kAddWays.rend(); ++way) {
        Item add = chrome(CommandItem(Kind::IconButton, {right - kKey, 0, right, kCaption}, way->name, way->glyph,
                                      way->command));
        add.tip = way->tip;
        keys.push_back(std::move(add));
        right -= kKey;
      }
    }
    // By the name: the app's own icon, its name, and the report beside them
    // (his: "сообщить о проблеме около иконки приложения").
    Item name = Make(Kind::AppTitle, {12, 0, right - 14, kCaption}, L"Sovereign");
    name.text2 = SelfPath();
    name.scrolls = false;
    float left = 12 + 24 + TextWidth(name.text, caption_.get()) + 6;
    Item report = chrome(CommandItem(Kind::IconButton, {left, 0, left + kKey, kCaption}, L"Сообщить о проблеме",
                                     kGlyphReport, UiCommand::ReportProblem));
    report.tip = L"Сообщить о проблеме: отчёт без ключей и адресов, его можно прочитать перед отправкой";
    left += kKey;
    // A new version: the pill after the report, solid accent with its
    // version on it where there's room, the arrow alone where there isn't
    // (DrawChrome). Downloading, a click opens the settings, where the
    // update's state is told in full. Waiting for a quiet moment, a click
    // installs it now.
    std::optional<Item> update;
    if (c.update == UiUpdate::Available || c.update == UiUpdate::Downloading || c.update == UiUpdate::Waiting) {
      const float wide = 4 + 22 + TextWidth(c.updateVersion, pill_.get()) + 12;
      const float w = left + wide <= right - 4 ? wide : kKey;
      update = chrome(CommandItem(Kind::IconButton, {left, 0, left + w, kCaption}, L"Обновить до " + c.updateVersion,
                                  kGlyphDownload, UiCommand::InstallUpdate));
      update->checked = true;
      if (w > kKey) {
        update->detail = c.updateVersion;
      }
      if (c.update == UiUpdate::Downloading) {
        update->action = ItemAction::Page;
        update->index = static_cast<int>(UiPage::Settings);
        update->tip = L"Скачиваю версию " + c.updateVersion + L"…";
      } else if (c.update == UiUpdate::Waiting) {
        update->tip = L"Версия " + c.updateVersion +
                      L" скачана и поставится, когда компьютер будет свободен. Нажми, чтобы поставить сейчас";
      } else if (!c.updateError.empty()) {
        update->tip = L"Обновление не удалось: " + c.updateError + L". Нажми, чтобы повторить";
      } else {
        update->tip = L"Доступна версия " + c.updateVersion + L". Нажми, чтобы обновить";
      }
    }
    name.rect.right = report.rect.left;
    l.items.push_back(std::move(name));
    // Left to right, as Tab goes: the report, the update, the page keys or "+", the cross.
    l.items.push_back(std::move(report));
    if (update) {
      l.items.push_back(std::move(*update));
    }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
      l.items.push_back(std::move(*it));
    }
    l.items.push_back(std::move(hide));
    l.items.push_back(std::move(close));
  }

  // The one screen for every day, top to bottom: the state, what waits for a
  // click (a new subscription), the switch, the server it runs on and
  // where it comes out. The rest of the window - routing, the checks, the
  // log, the kill switch - is in the settings, a key in the title bar.
  float Overview(Layout& l, const UiContent& c, float x0, float x1, float y, float bottom) const {
    // The state is the cube's: words only when there's something to say.
    // What's wrong is one quiet line, the whole of it in the log (Issues).
    if (const std::vector<Issue> issues = Issues(c); !issues.empty()) {
      std::wstring brief = issues.front().brief;
      if (issues.size() > 1) {
        brief += std::format(L" и ещё {}", issues.size() - 1);
      }
      Item line = Make(Kind::Banner, {x0, y, x1, y + 40}, brief, kGlyphWarning);
      line.checked = true;
      line.action = ItemAction::Page;
      line.index = static_cast<int>(UiPage::Logs);
      line.tip = L"Подробности — в журнале";
      l.items.push_back(std::move(line));
      y += 40 + 8;
    } else if (const std::wstring detail = StateDetail(c); !detail.empty()) {
      const float h = TextHeight(detail, captionWrap_.get(), x1 - x0);
      l.items.push_back(Make(Kind::Caption, {x0, y, x1, y + h}, detail));
      y += h + kGap;
    }

    // What waits for a click: one line each (a new version: a key in the title bar).
    if (AnyWaiting(c)) {
      Item waiting = Make(Kind::Banner, {x0, y, x1, y + 40}, L"Новая версия подписки — выбрать", kGlyphSync);
      waiting.action = ItemAction::Page;
      waiting.index = static_cast<int>(UiPage::Subscription);
      l.items.push_back(std::move(waiting));
      y += 40 + 8;
    }

    // Locked out: on, the kill switch holding the internet shut, nothing
    // getting through (main.cpp UpdateLockout) - why, and the way out.
    if (c.lockedOut) {
      l.items.push_back(CommandItem(Kind::Banner, {x0, y, x1, y + 40}, L"Открыть интернет без VPN", kGlyphWarning,
                                    UiCommand::OpenWithoutVpn));
      y += 40 + 8;
    }

    y = SubscriptionCard(l, c, x0, x1, y);

    // Nothing to connect with yet: the way to fix that comes first.
    // The first screen: what the clipboard holds is offered as it is ("Добавить
    // подписку с packetlab.tech"); else how to bring one - pasted, a QR code on
    // the screen, a file.
    if (c.profiles.empty()) {
      const bool offer = !c.clipboardOffer.empty();
      const std::wstring hint = offer ? L"В буфере обмена есть " + c.clipboardOffer + L"."
                                      : L"Скопируй ссылку подписки, ключ или конфиг — клиент найдёт его в буфере сам.";
      const float hintH = TextHeight(hint, captionWrap_.get(), x1 - x0 - 32);
      const float h = 12 + 26 + hintH + 12 + kButton + 8 + kButton + 14;
      const float mid = (x0 + x1) / 2;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      l.items.push_back(Make(Kind::Text, {x0 + 16, y + 12, x1 - 16, y + 38}, L"Добавь подписку"));
      float top = Paragraph(l, Kind::Caption, hint, x0 + 16, x1 - 16, y + 38) + 12;
      l.items.push_back(CommandItem(Kind::AccentButton, {x0 + 16, top, x1 - 16, top + kButton},
                                    offer ? L"Добавить " + c.clipboardOffer : std::wstring(L"Вставить из буфера"),
                                    kGlyphPaste, UiCommand::PasteSubscription));
      top += kButton + 8;
      Item qr = CommandItem(Kind::Button, {x0 + 16, top, mid - 4, top + kButton}, L"QR-код с экрана", kGlyphQrCode,
                            UiCommand::ScanScreen);
      qr.tip = L"Найти на экране QR-код подписки и добавить её";
      l.items.push_back(std::move(qr));
      l.items.push_back(
          CommandItem(Kind::Button, {mid + 4, top, x1 - 16, top + kButton}, L"Из файла…", kGlyphFile, UiCommand::ImportFile));
      y += h + kGap;
    }

    // The server at the foot; the switch in the middle of what's between,
    // big, a light behind it (DrawBackdrop) and the live traffic round it -
    // no words: its light and the streams say the state.
    // The switch's room no more than its least size wants: more, and two
    // subscriptions on a short window made the page scroll by a few pixels.
    constexpr float kServerCard = 60;
    constexpr float kLeastCube = 88;
    const float serverTop = std::max(y + kLeastCube + 40, bottom - kServerCard);
    const float room = serverTop - y;
    const float size = std::clamp(std::min(room - 40, (x1 - x0) * 0.6f), kLeastCube, 220.0f);
    const float top = y + std::max(0.0f, (room - size) / 2);
    const float mid = (x0 + x1) / 2;
    const D2D1_RECT_F cube{mid - size / 2, top, mid + size / 2, top + size};
    Item power = CommandItem(Kind::Power, cube, {}, kGlyphPower, UiCommand::Toggle);
    power.checked = c.on;
    power.tip = c.on ? L"Отключить VPN" : L"Подключить VPN";
    l.items.push_back(std::move(power));
    l.glow = cube;
    return ServerCard(l, c, x0, x1, serverTop);
  }

  // What it connects with, over the switch: a card of glass that is a link
  // to the configurations (refreshed, added, switched there; "+" in the
  // title bar adds one too) - every configuration on, the subscriptions
  // first, a line each: its name, till when it's paid (the panel's word, or
  // the user's for their own server: a click - paid again, another date),
  // and only what's wrong besides (a failed refresh, the traffic running
  // out). No buttons: the card says, it doesn't ask. Nothing before there's
  // one on.
  float SubscriptionCard(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    std::vector<std::size_t> shown;
    for (const bool subscription : {true, false}) {
      for (std::size_t i = 0; i < c.profiles.size(); ++i) {
        if (c.profiles[i].enabled && c.profiles[i].subscription == subscription) {
          shown.push_back(i);
        }
      }
    }
    if (shown.empty()) {
      return y;
    }
    // A line's height: the name and the paid line, and a line more for each thing wrong.
    constexpr float kHeader = 40;
    const auto rowHeight = [&](const UiProfile& p) {
      return 6.0f + 26 + 22 + (p.failed ? 20.0f : 0.0f) + (p.trafficShare >= 0.8f ? 20.0f : 0.0f) + 6;
    };
    float h = kHeader + 1;
    for (const std::size_t i : shown) {
      h += rowHeight(c.profiles[i]);
    }
    const float left = x0 + 20;  // the content's line (law.md, Grid Logic)
    const float right = x1 - 20;

    Item card = Make(Kind::Card, {x0, y, x1, y + h}, L"Конфигурации");
    card.action = ItemAction::Page;
    card.index = static_cast<int>(UiPage::Subscription);
    card.glass = true;
    card.tip = L"Конфигурации: обновить, выключить отдельные серверы";
    l.items.push_back(std::move(card));

    // The head: how many, and the three ways to add another configuration
    // (the clipboard, a QR code, a file) as keys, each with its tip (he
    // wanted adding here 16.10.2026; the "+ Добавить" and its list gone,
    // 30.10.2026).
    constexpr float kAddKey = 32;
    constexpr float kAddStep = kAddKey + 2;
    const float addWidth = kAddStep * static_cast<float>(kAddWays.size()) - 2;
    // The last key's glyph (16) ends on the line 20: its zone reaches past it.
    const float keysRight = right + (kAddKey - 16) / 2;
    float addLeft = keysRight - addWidth;
    for (const AddWay& way : kAddWays) {
      Item add = CommandItem(Kind::IconButton, {addLeft, y + 4, addLeft + kAddKey, y + kHeader - 4}, way.name, way.glyph,
                             way.command);
      add.tip = way.tip;
      l.items.push_back(std::move(add));
      addLeft += kAddStep;
    }
    Item label = Make(Kind::Muted, {left, y + 9, keysRight - addWidth - 8, y + kHeader - 9},
                      std::format(L"КОНФИГУРАЦИИ · {}", shown.size()));
    label.engraved = true;
    l.items.push_back(std::move(label));
    l.items.push_back(Make(Kind::Divider, {left, y + kHeader, right, y + kHeader + 1}));

    float top = y + kHeader + 1;
    for (const std::size_t i : shown) {
      const auto index = static_cast<int>(i);
      const UiProfile& p = c.profiles[i];
      const float rowTop = top;
      const float rowHeightNow = rowHeight(p);
      if (i != shown.front()) {
        l.items.push_back(Make(Kind::Divider, {left, rowTop, right, rowTop + 1}));
      }
      // Right to left: the arrow, the days left, and the name stops short of them.
      const float arrow = right - 20;
      l.items.push_back(Make(Kind::IconButton, {arrow, rowTop, right, rowTop + rowHeightNow}, {}, kGlyphChevron));
      float nameRight = arrow - 8;
      if (p.daysLeft >= 0) {
        const std::wstring days = p.daysLeft == 0 ? std::wstring(L"закончилась")
                                                  : std::format(L"{} {}", p.daysLeft, DaysWord(p.daysLeft));
        const float w = TextWidth(days, caption_.get()) + 2 * kPillPad;
        const float mid = rowTop + rowHeightNow / 2;
        Item pill = Make(Kind::Pill, {arrow - 8 - w, mid - 11, arrow - 8, mid + 11}, days);
        pill.warn = p.daysLeft <= 3;
        nameRight = pill.rect.left - 8;
        l.items.push_back(std::move(pill));
      }
      // The name, and what kind of configuration it is after it - if there's room.
      const float nameWidth = std::min(TextWidth(p.name, heading_.get()) + 2, nameRight - left);
      l.items.push_back(Make(Kind::Heading, {left, rowTop + 6, left + nameWidth, rowTop + 32}, p.name));
      const std::wstring badge = p.subscription ? L"подписка" : L"свой конфиг";
      const float badgeWidth = TextWidth(badge, caption_.get()) + 2 * kPillPad;
      if (left + nameWidth + 8 + badgeWidth <= nameRight) {
        l.items.push_back(Make(Kind::Pill, {left + nameWidth + 8, rowTop + 8, left + nameWidth + 8 + badgeWidth, rowTop + 30},
                               badge));
      }
      float line = rowTop + 34;
      if (p.paidTill.empty()) {
        // Not said by its panel, not by the user yet: a click to say it.
        const std::wstring ask = L"Указать, до какого числа оплачено ›";
        const float w = std::min(TextWidth(ask, caption_.get()) + 4, right - left);
        l.items.push_back(
            CommandItem(Kind::Muted, {left, line, left + w, line + 20}, ask, nullptr, UiCommand::PaidMenu, index));
      } else {
        std::wstring paid = (p.paidByHand ? L"Сервер оплачен до " : L"Оплачено до ") + p.paidTill.substr(0, 5);
        if (p.paidByHand) {
          paid += L" ›";
          const float w = std::min(TextWidth(paid, caption_.get()) + 4, right - left);
          Item item = CommandItem(Kind::Muted, {left, line, left + w, line + 20}, paid, nullptr, UiCommand::PaidMenu, index);
          item.warn = p.daysLeft <= 3;
          item.tip = L"Оплатил — продлить на месяц или указать дату";
          l.items.push_back(std::move(item));
        } else {
          Item item = Make(Kind::Muted, {left, line, right, line + 20}, paid);
          item.warn = p.daysLeft <= 3;
          l.items.push_back(std::move(item));
        }
      }
      line += 22;
      if (p.failed) {
        Item failed = Make(Kind::Muted, {left, line, right, line + 20}, L"Подписка не обновилась");
        failed.warn = true;
        l.items.push_back(std::move(failed));
        line += 20;
      }
      if (p.trafficShare >= 0.8f) {
        Item traffic =
            Make(Kind::Muted, {left, line, right, line + 20}, L"Трафик: " + p.trafficUsed + L" из " + p.trafficTotal);
        traffic.warn = true;
        l.items.push_back(std::move(traffic));
      }
      top += rowHeightNow;
    }
    return y + h + kGap;
  }

  // The server the box runs on as a card: the flag of where it really is,
  // its name, its network and latency; a click opens the servers. With auto,
  // the server auto picked.
  float ServerCard(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    const float h = 60;
    Item glass = Make(Kind::Card, {x0, y, x1, y + h});
    glass.glass = true;
    l.items.push_back(std::move(glass));
    Item card = Make(Kind::ServerCard, {x0 + 4, y + 2, x1 - 4, y + h - 2}, {}, kGlyphChevron);
    card.action = ItemAction::Page;
    card.index = static_cast<int>(UiPage::Servers);
    if (c.protocols.empty()) {
      // No servers (every configuration or server switched off): the card
      // leads to the configurations, where they are switched on again - not a
      // dead end with nothing to click.
      card.index = static_cast<int>(UiPage::Subscription);
      card.text = c.profiles.empty() ? L"Нет серверов" : L"Выбора сервера нет";
      card.detail = c.profiles.empty() ? L"добавь конфигурацию" : L"включи конфигурацию с серверами";
      l.items.push_back(std::move(card));
      return y + h;
    }
    // The server: the pick, or what auto runs on.
    int shown = c.protocol;
    const bool isAuto = c.protocol >= 0 && c.protocol == c.autoOption;
    if (isAuto && !c.autoServer.empty()) {
      const auto it = std::find(c.protocols.begin(), c.protocols.end(), c.autoServer);
      shown = it == c.protocols.end() ? c.protocol : static_cast<int>(it - c.protocols.begin());
    }
    const auto i = static_cast<std::size_t>(std::max(shown, 0));
    std::wstring name = shown >= 0 && i < c.protocols.size() ? OptionTitle(c, i) : std::wstring(L"выбор сервера");
    const UiLocation* location = shown >= 0 && i < c.locations.size() ? &c.locations[i] : nullptr;
    std::wstring country = location != nullptr ? location->country : std::wstring();
    if (country.empty() && c.display == Display::On) {
      country = c.exitCountry;  // where it comes out, until its own lookup is in
    }
    if (country.size() == 2) {
      if (const std::wstring flag =
              FlagEmojiFor(std::string{static_cast<char>(country[0]), static_cast<char>(country[1])});
          !flag.empty()) {
        name = flag + L" " + WithoutFlagEmoji(name);
      }
    }
    std::vector<std::wstring> parts;
    if (isAuto) {
      parts.emplace_back(c.autoServer.empty() ? L"авто: выберет после замера" : L"авто");
    }
    // Where the traffic comes out - unless hidden (the right-click menu).
    if (c.display == Display::On && !c.hideExitIp) {
      if (!c.exitIp.empty()) {
        parts.push_back(c.exitIp);
      } else if (c.exitPending) {
        parts.emplace_back(L"определяю IP…");
      }
    }
    if (location != nullptr && !location->isp.empty()) {
      parts.push_back(location->isp);
    }
    if (static_cast<std::size_t>(c.protocol) < c.delays.size()) {
      if (std::wstring delay = DelayLabel(c.delays[static_cast<std::size_t>(c.protocol)], c.delaysDetailed); !delay.empty()) {
        parts.push_back(std::move(delay));
      }
    }
    std::wstring detail;
    for (const std::wstring& part : parts) {
      detail += (detail.empty() ? L"" : L" · ") + part;
    }
    card.text = std::move(name);
    card.detail = detail.empty() ? std::wstring(L"нажми, чтобы выбрать сервер") : std::move(detail);
    card.tip = L"Выбрать сервер; авто сам берёт самый быстрый";
    l.items.push_back(std::move(card));
    return y + h;
  }

  // What option `i` is (UiContent::protocolTitles), or its name if that's unknown.
  static std::wstring OptionTitle(const UiContent& c, std::size_t i) {
    if (i < c.protocolTitles.size() && !c.protocolTitles[i].empty()) {
      return c.protocolTitles[i];
    }
    return i < c.protocols.size() ? c.protocols[i] : std::wstring();
  }

  // The servers of the subscription's selector, with their latency.
  float Servers(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    const float titleTop = y;
    std::wstring note;
    if (!c.selectError.empty()) {
      note = L"Не удалось переключить сервер: " + c.selectError;
    } else if (c.delaysTesting) {
      note = L"Проверяю задержку…";
    } else if (!c.canTestDelays && !c.protocols.empty()) {
      note = L"Задержку можно проверить, когда подключение включено.";
    } else if (!c.delayError.empty()) {
      note = L"Не удалось проверить: " + c.delayError;
    }
    y = PageTitle(l, L"Серверы", note.c_str(), x0, x1, y, UiPage::Overview,
                  L"Флаг и провайдер — где сервер на самом деле: его выход, узнанный через него самого, а не то, "
                  L"что написано в названии.\n\nЗадержка — медиана нескольких запросов через уже открытое "
                  L"соединение, округлённая до 5 мс: разница меньше — шум сети. «Подробные замеры» внизу "
                  L"покажут разброс (±), число запросов и время соединения. Авто берёт лучший по замерам и "
                  L"меняет его, только когда другой заметно быстрее два замера подряд.\n\nПравый клик по "
                  L"серверу — копировать имя или IP выхода.",
                  x1 - 40);
    Item test = CommandItem(Kind::IconButton, {x1 - 36, titleTop, x1, titleTop + 36}, L"Проверить задержку",
                            kGlyphStopwatch, UiCommand::TestDelays);
    // Always to press: with the connection off it says so in the note, while
    // one runs a press changes nothing.
    test.tip = L"Замерить задержку до всех серверов";
    l.items.push_back(std::move(test));
    if (c.protocols.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 56}));
      l.items.push_back(Make(Kind::Muted, {x0 + 16, y, x1 - 16, y + 56},
                             c.hasConfig ? L"В конфиге нет выбора сервера." : L"Ни одна конфигурация не включена."));
      return y + 56;
    }
    const auto row = [&](std::size_t i, float top) {
      // Where the server is, once known: its country's flag in place of any
      // its name has, and the network it's in ahead of the measurement.
      const UiLocation* location = i < c.locations.size() ? &c.locations[i] : nullptr;
      const std::wstring flag = location != nullptr && location->country.size() == 2
                                    ? FlagEmojiFor(std::string{static_cast<char>(location->country[0]),
                                                               static_cast<char>(location->country[1])})
                                    : std::wstring();
      const std::wstring title = OptionTitle(c, i);
      const std::wstring name = flag.empty() ? title : flag + L" " + WithoutFlagEmoji(title);
      std::wstring isp = location != nullptr ? location->isp : std::wstring();
      if (i < c.protocolNotes.size() && !c.protocolNotes[i].empty()) {
        std::wstring note = WithoutFlagEmoji(c.protocolNotes[i]);
        note.erase(0, note.find_first_not_of(L' '));
        isp += (isp.empty() ? L"" : L" · ") + note;
      }
      Item choice = CommandItem(Kind::Choice, {x0 + 4, top + 3, x1 - 4, top + kLatencyRow - 3}, name, nullptr,
                                UiCommand::SetProtocol, static_cast<int>(i));
      choice.checked = static_cast<int>(i) == c.protocol;
      if (static_cast<int>(i) == c.autoOption) {
        const auto at = std::find(c.protocols.begin(), c.protocols.end(), c.autoServer);
        const std::wstring server =
            at == c.protocols.end() ? c.autoServer : OptionTitle(c, static_cast<std::size_t>(at - c.protocols.begin()));
        choice.detail = c.autoServer.empty() ? std::wstring(L"выберет лучший после первого замера")
                                             : c.autoIsBest
                                                   ? L"сейчас " + server + L" — лучший по замерам"
                                                   : L"сейчас " + server + L" — другой сервер чуть быстрее, но разница мала";
      } else if (i < c.delays.size()) {
        choice.detail = DelayDetail(c.delays[i], c.delaysDetailed);
      }
      if (!isp.empty() && static_cast<int>(i) != c.autoOption) {
        choice.detail = choice.detail.empty() ? isp : isp + L" · " + choice.detail;
      }
      l.items.push_back(std::move(choice));
    };
    // Auto on its own card above the servers: it isn't one of them, it's a
    // way to pick one.
    const bool hasAuto = c.autoOption >= 0 && static_cast<std::size_t>(c.autoOption) < c.protocols.size();
    if (hasAuto) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + kLatencyRow}));
      row(static_cast<std::size_t>(c.autoOption), y);
      y += kLatencyRow + kGap;
    }
    const std::vector<std::size_t> order = c.serverOrder.empty() ? ServerOrder(c) : c.serverOrder;
    if (order.empty()) {
      return y - kGap;
    }
    if (order.size() > 1) {
      // Fastest first, by the last test, or as the subscription lists them.
      const float mid = (x0 + x1) / 2;
      Item fastest = CommandItem(Kind::Segment, {x0, y, mid - 4, y + 32}, L"Быстрые сверху", nullptr,
                                 UiCommand::SetServerOrder, 0);
      fastest.checked = c.serversByDelay;
      Item listed = CommandItem(Kind::Segment, {mid + 4, y, x1, y + 32}, L"Как в подписке", nullptr,
                                UiCommand::SetServerOrder, 1);
      listed.checked = !c.serversByDelay;
      l.items.push_back(std::move(fastest));
      l.items.push_back(std::move(listed));
      y += 32 + kGap;
    }
    const float h = static_cast<float>(order.size()) * kLatencyRow;
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
    std::size_t shown = 0;
    for (const std::size_t i : order) {
      if (i >= c.protocols.size()) {
        continue;
      }
      const float top = y + static_cast<float>(shown) * kLatencyRow;
      if (shown > 0) {
        l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
      }
      row(i, top);
      ++shown;
    }
    // The figures in full, or one rounded: noise to most, a tool to some.
    const std::vector<SwitchRow> details = {
        Row(L"Подробные замеры", L"Цифры за словом «стабильный»: ± разброс, сколько было запросов, время соединения; "
                                 L"ошибка сервера как её вернуло ядро.",
            c.delaysDetailed, UiCommand::ToggleDelayDetails)};
    return Switches(l, details, x0, x1, y + h + kGap);
  }

  float Subscription(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = PageTitle(l, L"Конфигурации", nullptr, x0, x1, y, UiPage::Overview,
                  L"Подписки, ключи и свои конфиги. Включённые работают вместе: их серверы — в общем списке "
                  L"«Серверы», маршруты и DNS — от первой включённой.\n\nНажми на конфигурацию, чтобы "
                  L"переименовать её, настроить обновление или выключить отдельные серверы. Правый клик — "
                  L"обновить, копировать ссылку, удалить.");

    // Every configuration: the switch turns it on and off, a click opens it.
    if (c.profiles.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 56}));
      l.items.push_back(Make(Kind::Muted, {x0 + 20, y, x1 - 20, y + 56},
                             L"Пока ни одной — добавь через «+» на главном экране."));
      y += 56;
    } else {
      const float h = static_cast<float>(c.profiles.size()) * kProfileRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t i = 0; i < c.profiles.size(); ++i) {
        const UiProfile& p = c.profiles[i];
        const float top = y + static_cast<float>(i) * kProfileRow;
        if (i > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
        }
        Item row = Make(Kind::ProfileRow, {x0 + 4, top + 3, x1 - 76, top + kProfileRow - 3}, p.name, kGlyphChevron);
        row.action = ItemAction::OpenProfile;
        row.index = static_cast<int>(i);
        row.detail = p.detail;
        row.checked = p.enabled;
        row.warn = p.failed;
        row.tip = L"Открыть: имя, обновление, серверы. Правый клик — меню";
        l.items.push_back(std::move(row));
        Item toggle = CommandItem(Kind::Toggle, {x1 - 72, top + 3, x1 - 8, top + kProfileRow - 3},
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

    // The subscription relay (relay.h), folded: the address and the key, each
    // typed over its row; the key is never shown. Folded, only whether it's
    // on - its address is the fold's. (In the settings till 17.10.2026: it's
    // how subscriptions download, so here.)
    {
      const auto row = [&](float top) { return D2D1_RECT_F{x0 + 4, top + 2, x1 - 4, top + kRow - 2}; };
      const auto line = [&](float top) { l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1})); };
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y}));
      const std::size_t card = l.items.size() - 1;
      const float fold = FoldOf(c.settingsMore, c.relayFold);
      Item relay = Make(Kind::ValueRow, row(y), L"Скачивать без подключения", kGlyphChevronDown);
      relay.action = ItemAction::ToggleMore;
      relay.fold = fold;
      if (fold < 0.5f) {  // the summary is the folded fold's; open, the rows say it
        relay.detail = !c.relayHost.empty() ? L"свой" : (c.sharedRelay ? L"Sovereign" : L"выключено");
      }
      l.items.push_back(std::move(relay));
      HintAfter(l, L"Скачивать без подключения", body_.get(), x0 + 20, x1 - 160, y + kRow / 2,
                L"Пока подключение работает, подписки скачиваются через него. Когда его нет, напрямую их сервер "
                L"увидел бы твой настоящий IP — поэтому они скачиваются через пересыльщик (Cloudflare Worker): сервер "
                L"подписки видит адрес Cloudflare, а ссылку на подписку — тот, кто держит пересыльщик.\n\n"
                L"«Через сервер Sovereign» — пересыльщик автора, ничего не настраивать. Хочешь, чтобы ссылки видел "
                L"только ты, — задай свой: адрес https://….workers.dev/ и ключ из relay.json, который пишет "
                L"tools/relay/deploy.ps1. Свой главнее.\n\nБез пересыльщика новую подписку предложат скачать "
                L"напрямую, остальные ждут подключения.");
      float top = y + kRow;
      if (fold > 0) {
        const std::size_t first = l.items.size();
        const float bodyTop = top;
        line(top);
        Item shared = CommandItem(Kind::Switch, {x0 + 4, top + 2, x1 - 4, top + 56 - 2}, L"Через сервер Sovereign", nullptr,
                                  UiCommand::ToggleSharedRelay);
        shared.detail = c.relayHost.empty() ? std::wstring(L"сервер видит ссылку на подписку")
                                            : std::wstring(L"пока задан свой — он главнее");
        shared.checked = c.sharedRelay;
        l.items.push_back(std::move(shared));
        top += 56;
        line(top);
        Item url = CommandItem(Kind::ValueRow, row(top), L"Свой адрес", nullptr, UiCommand::EditRelayUrl);
        url.detail = c.relayHost.empty() ? std::wstring(L"не задан") : c.relayHost;
        l.items.push_back(std::move(url));
        top += kRow;
        line(top);
        Item key = CommandItem(Kind::ValueRow, row(top), L"Свой ключ", nullptr, UiCommand::EditRelayKey);
        key.detail = c.relaySet ? L"задан" : L"не задан";
        key.enabled = !c.relayHost.empty();
        l.items.push_back(std::move(key));
        top = FoldBody(l, first, bodyTop, top + kRow, fold);
      }
      l.items[card].rect.bottom = top;
      y = top;
    }
    // Adding one is the main screen's: its "+" (the clipboard, a file, a QR code).
    return y;
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
        it->tip = L"Нажми, чтобы переименовать";
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
      l.items.push_back(Make(Kind::Divider, {x0 + 20, y + 64, x1 - 20, y + 65}));
      Item autoUpdate = CommandItem(Kind::Switch, {x0 + 4, y + 68, x1 - 4, y + 124}, L"Автообновление", nullptr,
                                    UiCommand::ToggleAutoUpdate, index);
      autoUpdate.detail = p.autoUpdate ? p.period : L"только кнопкой «Обновить»";
      autoUpdate.checked = p.autoUpdate;
      l.items.push_back(std::move(autoUpdate));
    }
    y += cardHeight + kGap;

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
          l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
        }
        Item field = Make(Kind::Field, {x0 + 20, top, x1 - 20, top + kRow}, fields[i].first);
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
      if (!p.usage.empty()) {
        y = Paragraph(l, Kind::Caption, p.usage, x0 + 4, x1, y) + kGap;
      }
      Item refresh =
          CommandItem(Kind::Button, {x0, y, mid - 4, y + kButton}, L"Обновить", kGlyphRefresh, UiCommand::RefreshProfile, index);
      refresh.tip = L"Скачать подписку заново сейчас";
      l.items.push_back(std::move(refresh));
      l.items.push_back(CommandItem(Kind::Button, {mid + 4, y, x1, y + kButton}, L"Копировать ссылку", kGlyphCopy,
                                    UiCommand::CopyProfileLink, index));
      y += kButton + 8;
      // What's wrong with which server, for the provider's support: a file,
      // a short version on the clipboard, their link opened if the panel gave one.
      l.items.push_back(ReportButton(c, p.support, {x0, y, x1, y + kButton}, index));
      y += kButton + 8;
      if (p.edited || p.waiting) {
        Item revert =
            CommandItem(Kind::Button, {x0, y, x1, y + kButton}, L"Как в подписке", kGlyphUndo, UiCommand::RevertConfig, index);
        revert.tip = L"Вернуть конфиг из подписки; твой останется в истории";
        l.items.push_back(std::move(revert));
        y += kButton + 8;
      }
    }
    Item rename = Make(Kind::Button, {x0, y, p.subscription ? mid - 4 : x1, y + kButton}, L"Переименовать",
                       kGlyphRename);
    rename.action = ItemAction::EditName;
    rename.index = index;
    l.items.push_back(std::move(rename));
    if (p.subscription) {
      Item period =
          CommandItem(Kind::Button, {mid + 4, y, x1, y + kButton}, L"Период…", kGlyphClock, UiCommand::ChooseRefreshPeriod, index);
      period.tip = L"Как часто обновлять подписку";
      l.items.push_back(std::move(period));
    }
    y += kButton + kGap + 4;

    // A chain (combine.h): its servers connect through another configuration's.
    {
      Item via = CommandItem(Kind::ValueRow, {x0, y, x1, y + kRow}, L"Подключаться через", nullptr,
                             UiCommand::ChooseVia, index);
      via.detail = p.via.empty() ? std::wstring(L"напрямую") : p.via;
      l.items.push_back(Make(Kind::Card, via.rect));
      via.rect = {x0 + 4, via.rect.top + 2, x1 - 4, via.rect.bottom - 2};
      l.items.push_back(std::move(via));
      HintAfter(l, L"Подключаться через", body_.get(), x0 + 20, x1 - 120, y + kRow / 2,
                L"Напрямую — серверы этой конфигурации видят твой настоящий IP. Через другую твою конфигурацию — "
                L"они видят адрес её сервера, а не твой: для сервисов, которым не хочется показывать себя.\n\n"
                L"Если та конфигурация выключена или удалена, серверы этой не подключаются вовсе — не напрямую.");
      y += kRow + 6;
      if (p.viaBroken) {
        y = Paragraph(l, Kind::ErrorText, L"«" + p.via + L"» выключена или удалена — серверы этой конфигурации не работают.",
                      x0, x1, y) + 6;
      }
      y += kGap;
    }

    // Its servers: a switch each.
    const auto off = std::count_if(p.servers.begin(), p.servers.end(), [](const UiServer& s) { return !s.enabled; });
    y = Heading(l,
                off > 0 ? std::format(L"Серверы: {}, выключено {}", p.servers.size(), off)
                        : std::format(L"Серверы: {}", p.servers.size()),
                x0, x1, y,
                L"Выключенный сервер не попадает в общий список и в авто. Правый клик — копировать его имя.");
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
          l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
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

  // Where traffic goes and how names resolve (routing.h), a section a card:
  // the traffic's switches (and the lists they use), DNS, WARP,
  // the user's rules - first, whatever the source.
  float Routing(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    const UiRouting& r = c.routing;
    y = PageTitle(l, L"Маршруты", nullptr, x0, x1, y, BackOf(UiPage::Routing),
                  L"Куда идёт трафик. Это решает Sovereign: из подписок берутся только серверы, их правила "
                  L"и DNS не используются.\n\n«Всё» — через VPN идёт всё, а "
                  L"ниже исключения: то, что идёт иначе. «Только выбранное» — через VPN идёт лишь то, что "
                  L"лежит в «Через VPN», остальное напрямую. Списки для обоих режимов хранятся: программы "
                  L"одного не видны в другом, потому что там ничего не меняют.");
    // The mode (RoutingSettings::finalDirect): everything through the VPN but
    // what's sent elsewhere, or only what's sent through it.
    {
      // Read with the segments: "Через VPN идёт: всё" - and the buckets under
      // it captioned as the exceptions to that ("Всё через VPN" over a list
      // of what doesn't, uncaptioned, was a lie; "всё, кроме исключений" on
      // the segment said both at once and outweighed the board).
      // Section headings as the settings' (30.10.2026, a review's "типографическая
      // шкала не системна": a label here, a heading there, for one role).
      y = Heading(l, L"Через VPN идёт", x0, x1, y);
      // One card holds the mode and what it comes to: "всё остальное → VPN"
      // (or "напрямую"), the way everything the board doesn't name goes. The
      // board under it is what goes otherwise ("Идёт иначе": WARP is no
      // exception to anything, 30.10.2026, his: "варп вряд ли исключением
      // можно назвать"), so the mode's own way isn't a card among them.
      // The law's lines (law.md, Grid Logic): the segments are plates, 8 from the card's edge and 8
      // apart; the line under them is content, its words on the line 20.
      constexpr float kModePad = 8;
      constexpr float kSegment = 32;
      constexpr float kRestLine = 24;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + kModePad + kSegment + 8 + kRestLine + kModePad}));
      const float mid = (x0 + x1) / 2;
      const float inLeft = x0 + 20;
      Item all = CommandItem(Kind::Segment, {x0 + kModePad, y + kModePad, mid - 4, y + kModePad + kSegment}, L"всё",
                             nullptr, UiCommand::SetRouteMode, 0);
      all.checked = !r.finalDirect;
      Item chosen = CommandItem(Kind::Segment, {mid + 4, y + kModePad, x1 - kModePad, y + kModePad + kSegment},
                                L"только выбранное", nullptr, UiCommand::SetRouteMode, 1);
      chosen.checked = r.finalDirect;
      l.items.push_back(std::move(all));
      l.items.push_back(std::move(chosen));
      const float lineTop = y + kModePad + kSegment + 8;
      const std::wstring lead = L"всё остальное →";
      const float leadWidth = TextWidth(lead, caption_.get());
      l.items.push_back(Make(Kind::Muted, {inLeft, lineTop, inLeft + leadWidth + 4, lineTop + kRestLine}, lead));
      const std::wstring way = r.finalDirect ? L"Напрямую" : L"VPN";
      Item restWay = Make(Kind::Pill, {inLeft + leadWidth + 8, lineTop + (kRestLine - kPillHeight) / 2,
                                       inLeft + leadWidth + 8 + TextWidth(way, caption_.get()) + kPillPad + 22,
                                       lineTop + (kRestLine + kPillHeight) / 2},
                          way);
      restWay.rail = r.finalDirect ? kBucketDirect : kBucketProxy;
      l.items.push_back(std::move(restWay));
      y += kModePad + kSegment + 8 + kRestLine + kModePad + kGap;
      y = Heading(l, L"Идёт иначе", x0, x1, y);
      // How an exception is made, said where it's made (31.10.2026, his: sending a site "напрямую"
      // meant finding the "+" of a way - "новички и не только не прохавают").
      y = Paragraph(l, Kind::Caption,
                    r.finalDirect ? L"Напрямую идёт всё, кроме того, что добавишь ниже. «+» у нужного пути — сайт, "
                                    L"сервис или программа."
                                  : L"Всё идёт через VPN, кроме того, что добавишь ниже. «+» у нужного пути — сайт, "
                                    L"сервис или программа.",
                    x0, x1, y) +
          4;
    }
    // Where everything goes: the board. (The fine settings - QUIC, IPv4, DNS -
    // are the settings' since 17.10.2026: nothing of where a site goes.)
    y = Board(l, r, x0, x1, y);
    if (!r.lists.empty()) {  // only while the lists are downloading, or failed
      y = Paragraph(l, r.listsFailed ? Kind::ErrorText : Kind::Caption, r.lists, x0 + 4, x1, y) + 4;
    }
    y += kGap;
    // Set up, then seen going: an address typed, or a tile's "Проверить" -
    // held to its bucket; a way out under it when it went elsewhere.
    if (c.checks.size() > kCheckRoute) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + kCheckRow}));
      CheckLine(l, c, kCheckRoute, x0, x1, y);
      y += kCheckRow;
      const UiCheck& route = c.checks[kCheckRoute];
      // An address was checked and seen (or not seen: the one that doesn't open): the ways it
      // can be sent instead, one tap each - a rule for it on the board (31.10.2026, his: the
      // check at the bottom should do something, not only say). The way it went isn't offered; nor
      // is WARP while it's off; blocking isn't a way to fix a site that won't open.
      if (!route.site.empty() && (route.status == UiCheck::Status::Ok || route.status == UiCheck::Status::Warn)) {
        std::vector<std::pair<int, const wchar_t*>> ways;
        for (const auto& [bucket, label] : {std::pair{kBucketDirect, L"Напрямую"}, std::pair{kBucketProxy, L"Через VPN"},
                                            std::pair{kBucketWarp, L"Через WARP"}}) {
          if (bucket != route.siteWay && (bucket != kBucketWarp || c.routing.warp)) {
            ways.emplace_back(bucket, label);
          }
        }
        if (!ways.empty()) {
          y += 8;
          constexpr float kSendPad = 8;
          const float inLeft = x0 + 20;
          const float inRight = x1 - 20;
          // The check row cuts a long address short: the label says what was checked and how it
          // goes now, so "иначе" has something to be other than.
          static constexpr std::array<const wchar_t*, 4> kNow = {L"идёт напрямую", L"идёт через VPN", L"заблокирован",
                                                                 L"идёт через WARP"};
          const std::wstring ask =
              L"«" + route.site + L"» " +
              (route.siteWay >= 0 && route.siteWay < 4 ? std::wstring(kNow.at(static_cast<std::size_t>(route.siteWay))) + L". Пустить:"
                                                       : std::wstring(L"— пустить иначе:"));
          const float askHeight = std::max(24.0f, TextHeight(ask, captionWrap_.get(), inRight - inLeft));
          const float cardHeight = kSendPad + askHeight + 32 + kSendPad;
          l.items.push_back(Make(Kind::Card, {x0, y, x1, y + cardHeight}));
          l.items.push_back(Make(Kind::Caption, {inLeft, y + kSendPad, inRight, y + kSendPad + askHeight}, ask));
          const float each = (inRight - inLeft - 8 * static_cast<float>(ways.size() - 1)) / static_cast<float>(ways.size());
          const float top = y + kSendPad + askHeight;
          for (std::size_t k = 0; k < ways.size(); ++k) {
            const float bx = inLeft + static_cast<float>(k) * (each + 8);
            Item send = CommandItem(Kind::Button, {bx, top, bx + each, top + 32}, ways[k].second, nullptr,
                                    UiCommand::RouteSite, ways[k].first);
            send.tip = L"Добавить сайт в этот путь на доске «Маршруты». Уберёшь там же.";
            l.items.push_back(std::move(send));
          }
          y += cardHeight;
        }
      }
      if (route.status == UiCheck::Status::Fail && route.fix != UiCheck::Fix::None) {
        const FixAction way = FixWay(route.fix, c);
        y += 8;
        Item banner = Make(Kind::Banner, {x0, y, x1, y + 40}, L"Не так, как на доске — " + way.label, kGlyphWarning);
        banner.action = way.action;
        banner.command = way.command;
        banner.index = way.index;
        l.items.push_back(std::move(banner));
        y += 40;
      }
      y += kGap;
    }
    // Only the listed programs through the VPN: a mode of its own (the
    // others are on the board).
    return c.appsInclude ? Apps(l, c, x0, x1, y) : y;
  }

  // The checks: what the internet sees, leaks, the local network, DNS, and -
  // on their own buttons - speed and where an address goes.
  float Checks(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    // "Check all" in the title's row; what the page is about in its "?".
    const float titleTop = y;
    const float allWidth = 140;
    y = PageTitle(l, L"Проверка", nullptr, x0, x1, y, BackOf(UiPage::Checks),
                  L"Запросы трея идут как у любой программы — по тем же правилам, поэтому видно то же, что "
                  L"видят твои программы и сайты.\n\n«Проверить всё» — все строки, кроме скорости и адреса: у них "
                  L"свои кнопки справа. Наведи на «?» у строки — что она проверяет и что значит результат. Правый "
                  L"клик — проверить снова или скопировать результат.",
                  x1 - allWidth - 8);
    Item all = CommandItem(Kind::AccentButton, {x1 - allWidth, titleTop + 1, x1, titleTop + 1 + kButton},
                           c.checksRunning ? L"Проверяю…" : L"Проверить всё", kGlyphShield, UiCommand::RunChecks);
    all.enabled = !c.checksRunning;
    l.items.push_back(std::move(all));

    // One card, a line each: status, title and "?", the result on the right.
    // Speed makes traffic: a button of its own. Where an address goes is the
    // routing page's - by the board it's checked against (16.10.2026).
    std::vector<std::size_t> shown;
    for (std::size_t i = 0; i < c.checks.size(); ++i) {
      if (i != kCheckRoute) {
        shown.push_back(i);
      }
    }
    const float h = static_cast<float>(shown.size()) * kCheckRow;
    l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
    for (std::size_t k = 0; k < shown.size(); ++k) {
      const float top = y + static_cast<float>(k) * kCheckRow;
      if (k > 0) {
        l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
      }
      CheckLine(l, c, shown[k], x0, x1, top);
    }
    y += h;

    // The speed test's graph, growing while it runs and kept after.
    if (c.checks.size() > kCheckSpeed &&
        (!c.checks[kCheckSpeed].speedDown.empty() || !c.checks[kCheckSpeed].speedUp.empty())) {
      y += 8;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + kSpeedGraph}));
      Item graph = Make(Kind::SpeedGraph, {x0, y, x1, y + kSpeedGraph});
      graph.index = static_cast<int>(kCheckSpeed);
      l.items.push_back(std::move(graph));
      y += kSpeedGraph;
    }

    // The failed ones' way out, under the results: a banner an action, the
    // checks it helps named in it (WebRTC and IPv6 both want the kill switch).
    std::vector<FixAction> ways;
    for (std::size_t i = 0; i < c.checks.size(); ++i) {
      const UiCheck& check = c.checks[i];
      if (i == kCheckRoute || check.status != UiCheck::Status::Fail || check.fix == UiCheck::Fix::None) {
        continue;  // the address's way out is under its row, by the board
      }
      FixAction way = FixWay(check.fix, c);
      const auto same =
          std::find_if(ways.begin(), ways.end(), [&](const FixAction& w) { return w.label == way.label; });
      if (same != ways.end()) {
        same->titles += L", " + check.title;
        continue;
      }
      way.titles = check.title;
      ways.push_back(std::move(way));
    }
    for (const FixAction& way : ways) {
      y += 8;
      Item banner = Make(Kind::Banner, {x0, y, x1, y + 40}, way.titles + L" — " + way.label, kGlyphWarning);
      banner.action = way.action;
      banner.command = way.command;
      banner.index = way.index;
      l.items.push_back(std::move(banner));
      y += 40;
    }
    return y;
  }

  // A check's line (c.checks[i]) at `top`: its status dot, its title and
  // "?" (what it does, and the last result in full), the result on the
  // right; the speed test and an address with a button of their own.
  void CheckLine(Layout& l, const UiContent& c, std::size_t i, float x0, float x1, float top) const {
    const UiCheck& check = c.checks[i];
    const bool hasButton = i == kCheckSpeed || i == kCheckRoute;
    Item row = Make(Kind::CheckRow, {x0 + 4, top + 1, x1 - 4, top + kCheckRow - 1}, check.title);
    // Not run, a dash - unless the row's own button says what to do (the route's
    // "—" read as a "minus" to fold, 30.10.2026).
    row.detail = check.status != UiCheck::Status::NotRun ? check.summary
                 : i == kCheckRoute                      ? std::wstring()
                                                         : std::wstring(L"—");
    row.index = static_cast<int>(check.status);
    row.sub = static_cast<int>(i);
    row.text2 = check.detail;
    row.checked = hasButton;  // the result stops short of the button
    l.items.push_back(std::move(row));

    std::wstring hint = check.hint;
    if (check.status != UiCheck::Status::NotRun && check.status != UiCheck::Status::Running) {
      hint += L"\n\nСейчас: " + check.summary;
      if (!check.detail.empty()) {
        hint += L"\n" + check.detail;
      }
    }
    // With a result, its room (120) is the "?"'s limit; with the word "Проверить" and no result - the
    // button's edge: at 350 wide the "?" sat on "сайт?" (31.10.2026).
    const bool word = i == kCheckRoute && check.status == UiCheck::Status::NotRun;
    const float hintRight = word ? x1 - 20 - kRouteButton - 6 : x1 - 120;
    HintAfter(l, check.title, body_.get(), x0 + 4 + 34, hintRight, top + kCheckRow / 2, std::move(hint));

    if (hasButton) {
      // The route's a word, not an arrow: an arrow there read as "unfold" (30.10.2026);
      // with a result in the room, "again" - the icons' refresh. The word's button ends on
      // the line 20, the key (28) in the zone that ends on 14: its glyph ends on 20.
      Item run = word ? CommandItem(Kind::Button, {x1 - 20 - kRouteButton, top + 4, x1 - 20, top + kCheckRow - 4},
                                    L"Проверить", nullptr, UiCommand::CheckRoute)
                      : CommandItem(Kind::IconButton, {x1 - 14 - 28, top + 7, x1 - 14, top + kCheckRow - 7},
                                    i == kCheckSpeed ? L"Замерить скорость" : L"Проверить другой сайт…",
                                    i == kCheckSpeed ? kGlyphSpeed : kGlyphRefresh,
                                    i == kCheckSpeed ? UiCommand::RunSpeed : UiCommand::CheckRoute);
      run.enabled = !c.checksRunning;
      run.tip = i == kCheckSpeed ? L"Замерить скорость приёма и отдачи через VPN"
                                 : L"Ввести сайт и узнать, каким путём он пойдёт";
      l.items.push_back(std::move(run));
    }
  }

  // A failed check's way out on the page: what the banner says, and what a
  // click does - a command, or a page opened; `titles`: the checks it helps.
  struct FixAction {
    std::wstring label;
    ItemAction action = ItemAction::Command;
    UiCommand command{};
    int index = 0;
    std::wstring titles;
  };

  // What the page offers for a failed check (UiCheck::Fix). The kill switch
  // already on, a leak is the connection's: it's restarted.
  static FixAction FixWay(UiCheck::Fix fix, const UiContent& c) {
    const auto page = [](const wchar_t* label, UiPage to) {
      return FixAction{
          .label = label, .action = ItemAction::Page, .command = {}, .index = static_cast<int>(to), .titles = {}};
    };
    const auto reconnect = [] {
      return FixAction{
          .label = L"переподключить", .action = ItemAction::Command, .command = UiCommand::Reconnect, .index = 0, .titles = {}};
    };
    switch (fix) {
      case UiCheck::Fix::KillSwitch:
        if (!c.killSwitch) {
          return FixAction{.label = L"включить Kill switch",
                     .action = ItemAction::Command,
                     .command = UiCommand::ToggleKillSwitch,
                     .index = kFromChecks,
                     .titles = {}};
        }
        return reconnect();
      case UiCheck::Fix::Routing: return page(L"открыть маршруты", UiPage::Routing);
      case UiCheck::Fix::Dns: return page(L"сменить DNS в маршрутах", UiPage::Routing);
      case UiCheck::Fix::LocalNetwork: return page(L"открыть настройки локальной сети", UiPage::Settings);
      case UiCheck::Fix::Reconnect:
      case UiCheck::Fix::None: break;
    }
    return reconnect();
  }

  // The per-app list - a section of Routing: which programs go through the proxy.
  float Apps(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = Heading(l, L"Приложения", x0, x1, y,
                L"Какие программы идут через VPN. Имена — как у exe-файла, без пути: правило ловит программу, "
                L"где бы она ни лежала.");
    const float mid = (x0 + x1) / 2;
    Item except = CommandItem(Kind::Segment, {x0, y, mid - 4, y + 36}, L"Все, кроме списка", nullptr,
                              UiCommand::SetAppsMode, 0);
    except.checked = !c.appsInclude;
    Item only = CommandItem(Kind::Segment, {mid + 4, y, x1, y + 36}, L"Только список", nullptr, UiCommand::SetAppsMode, 1);
    only.checked = c.appsInclude;
    l.items.push_back(std::move(except));
    l.items.push_back(std::move(only));
    y += 36 + 6;
    y = Paragraph(l, Kind::Caption,
                  c.appsInclude ? L"Через VPN идут только программы из списка, остальной трафик — напрямую."
                                : L"Весь трафик идёт через VPN, кроме программ из списка — они ходят напрямую.",
                  x0 + 4, x1, y) +
        8;

    if (c.apps.empty()) {
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + 64}));
      l.items.push_back(Make(Kind::Muted, {x0 + 16, y, x1 - 16, y + 64},
                             c.appsInclude ? L"Список пуст — через VPN не идёт ничего." : L"Список пуст — всё идёт через VPN."));
      y += 64;
    } else {
      const float h = static_cast<float>(c.apps.size()) * kRow;
      l.items.push_back(Make(Kind::Card, {x0, y, x1, y + h}));
      for (std::size_t i = 0; i < c.apps.size(); ++i) {
        const float top = y + static_cast<float>(i) * kRow;
        if (i > 0) {
          l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
        }
        Item app = Make(Kind::AppRow, {x0 + 8, top, x1 - 56, top + kRow}, c.apps[i], kGlyphProgram);
        app.detail = i < c.appPaths.size() ? c.appPaths[i] : std::wstring();  // the icon's exe
        app.index = static_cast<int>(i);  // for its right-click menu
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
    y = PageTitle(l, L"Журнал", nullptr, x0, x1, y, BackOf(UiPage::Logs));
    float right = x1;
    Item save = Make(Kind::IconButton, {right - 36, titleTop, right, titleTop + 36}, L"Сохранить в файл", kGlyphSave);
    save.action = ItemAction::SaveLogs;
    save.tip = L"Сохранить журнал в файл";
    l.items.push_back(std::move(save));
    right -= 40;
    const bool waiting = s.paused && s.held > 0;
    const float pauseWidth = waiting ? 84.0f : 36.0f;
    Item pause = Make(waiting ? Kind::Button : Kind::IconButton, {right - pauseWidth, titleTop, right, titleTop + 36},
                      waiting ? std::format(L"+{}", s.held) : std::wstring(s.paused ? L"Продолжить" : L"Пауза"),
                      s.paused ? kGlyphPlay : kGlyphPause);
    pause.action = ItemAction::PauseLogs;
    pause.checked = s.paused;
    pause.tip = waiting    ? std::format(L"Новых строк: {}. Показать их и продолжить", s.held)
                : s.paused ? L"Продолжить: новые строки снова пойдут"
                           : L"Пауза: новые строки подождут, пока читаешь";
    l.items.push_back(std::move(pause));
    right -= pauseWidth + 4;
    Item writes = CommandItem(Kind::Button, {right - 124, titleTop + 1, right, titleTop + 1 + kButton},
                              c.logLevel.empty() ? std::wstring(L"запись: авто") : L"запись: " + c.logLevel,
                              kGlyphFilter, UiCommand::ChooseLogLevel);
    writes.tip = L"Насколько подробно ядро пишет журнал";
    l.items.push_back(std::move(writes));

    // What's wrong right now, in full and in red, over the lines (the main
    // screen has one line of it); the subscription's report under its trouble.
    for (const Issue& issue : Issues(c)) {
      y = Paragraph(l, Kind::ErrorText, issue.full, x0, x1, y) + 8;
    }
    if (!c.trouble.empty() && c.troubleProfile >= 0) {
      l.items.push_back(ReportButton(c, c.troubleSupport, {x0, y, x1, y + kButton}, c.troubleProfile));
      y += kButton + kGap;
    }

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
        s.detail.empty() ? 0.0f
                         : std::min(std::max(84.0f, height * 0.3f), TextHeight(s.detail, captionWrap_.get(), x1 - x0 - 28)) + 20;
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

  // The fine settings, the settings' fold's body: the seam (a groove, the
  // law's disclosure grammar), then QUIC, IPv4 and DNS, a row each, in the
  // handle's own card - the fold opens in place, not in a card under it
  // (31.10.2026). `top` is where the handle ends; returns where the last row does.
  // (The DNS had a heading and a card of its own till 30.10.2026, his: the
  // settings didn't fit a screen. Hysteria 2's speed is its servers' menu's
  // since 17.10.2026.)
  float FineRows(Layout& l, const UiContent& c, float x0, float x1, float top) const {
    const UiRouting& r = c.routing;
    {
      l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
      Item quic = CommandItem(Kind::ValueRow, {x0 + 4, top + 2, x1 - 4, top + kSettingsRow - 2}, L"QUIC", nullptr,
                              UiCommand::ChooseQuic);
      quic.detail = r.quic == 1     ? L"выкл."
                    : r.quic == 2   ? L"вкл."
                    : r.quicBlocked ? L"авто (сейчас выкл.)"
                                    : L"авто (сейчас вкл.)";
      quic.hinted = true;
      l.items.push_back(std::move(quic));
      HintAfter(l, L"QUIC", body_.get(), x0 + 20, x1 - 8, top + kSettingsRow / 2,
                L"QUIC — HTTP/3 по UDP. Через серверы на TCP (VLESS, Trojan, AnyTLS) он идёт хуже обычного TCP, "
                L"поэтому выключенный QUIC не мешает: браузеры сразу берут TCP.\n\n«Авто» включает QUIC, только "
                L"если все твои серверы — Hysteria2 или TUIC. Есть хоть один на TCP — выключен.");
      top += kSettingsRow;
      l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
      Item v4 = CommandItem(Kind::Switch, {x0 + 4, top + 2, x1 - 4, top + kSettingsRow - 2}, L"Только IPv4", nullptr,
                            UiCommand::ToggleIpv4Only);
      v4.checked = r.ipv4Only;
      l.items.push_back(std::move(v4));
      HintAfter(l, L"Только IPv4", body_.get(), x0 + 20, x1 - 80, top + kSettingsRow / 2,
                L"Сайты получают только IPv4-адреса. У многих серверов VPN нет IPv6: без этого браузер сначала "
                L"пробует IPv6, ждёт и лишь потом идёт по IPv4 — сайт открывается с задержкой или не открывается.");
      top += kSettingsRow;

      // DNS: who answers for the names of each kind - auto, a pick, or a
      // server of the user's own (typed over its row).
      l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
      Item remote = CommandItem(Kind::ValueRow, {x0 + 4, top + 2, x1 - 4, top + kSettingsRow - 2}, L"DNS через VPN",
                                nullptr, UiCommand::ChooseRemoteDns);
      remote.detail = r.remoteDns;
      remote.hinted = true;
      remote.tick = kBucketProxy;
      remote.tickAlways = true;
      l.items.push_back(std::move(remote));
      HintAfter(l, L"DNS через VPN", body_.get(), x0 + 20, x1 - 8, top + kSettingsRow / 2,
                L"Имена всех сайтов, кроме прямых: запрос идёт через VPN, провайдер его не видит.\n\n«Авто» "
                L"подходит почти всем: это Cloudflare. Свой адрес пишется так: 1.2.3.4, tls://dns.example, "
                L"https://dns.example/dns-query, quic://dns.example.");
      top += kSettingsRow;
      l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1}));
      Item local = CommandItem(Kind::ValueRow, {x0 + 4, top + 2, x1 - 4, top + kSettingsRow - 2}, L"DNS напрямую",
                               nullptr, UiCommand::ChooseLocalDns);
      local.detail = r.localDns;
      local.hinted = true;
      local.tick = kBucketDirect;
      local.tickAlways = true;
      l.items.push_back(std::move(local));
      HintAfter(l, L"DNS напрямую", body_.get(), x0 + 20, x1 - 8, top + kSettingsRow / 2,
                L"Российские сайты, твои прямые и адреса самих серверов.\n\nЕсли этот DNS перестанет отвечать, "
                L"Sovereign возьмёт следующий: Cloudflare, Google, Quad9, Яндекс, в конце DNS Windows. Яндекс — "
                L"только здесь: в России его не блокируют.");
      top += kSettingsRow;
    }
    return top;
  }

  float Settings(Layout& l, const UiContent& c, float x0, float x1, float y) const {
    y = PageTitle(l, L"Настройки", nullptr, x0, x1, y, UiPage::Overview,
                  L"Крестик закрывает клиент совсем, чёрточка прячет окно в трей. Подключение остаётся и после "
                  L"крестика: им управляет служба.\n\nВсё настроенное здесь, конфиги и их история лежат в «Папке "
                  L"данных» внизу — %LOCALAPPDATA%\\Sovereign.");

    // Only what's set: where traffic goes (a page of its own), how the
    // connection behaves (it holds when the VPN drops, it starts with Windows,
    // the local network shut), the fine settings folded. Rows in a card a line apart: the
    // card goes in first (drawn under its rows), its foot set once they're
    // in. The rest went where it belongs (17.10.2026, he: "не стакаются"):
    // the problem report to the title bar, the subscription relay to the
    // configurations, quitting to the tray's menu; the version and the data
    // folder are the page's foot.
    const auto open = [&](float top) {
      l.items.push_back(Make(Kind::Card, {x0, top, x1, top}));
      return l.items.size() - 1;
    };
    const auto close = [&](std::size_t card, float bottom) {
      l.items[card].rect.bottom = bottom;
      return bottom + kSettingsGap;
    };
    const auto line = [&](float top) { l.items.push_back(Make(Kind::Divider, {x0 + 20, top, x1 - 20, top + 1})); };
    const auto row = [&](float top) { return D2D1_RECT_F{x0 + 4, top + 2, x1 - 4, top + kSettingsRow - 2}; };

    {
      const std::size_t card = open(y);
      // The routing is not here: the title bar's key opens it (30.10.2026, his: "на главной пусть будут").
      Item checks = CommandItem(Kind::ValueRow, row(y), L"Проверка подключения", nullptr, UiCommand::OpenChecks);
      checks.detail = L"утечки, скорость";
      l.items.push_back(std::move(checks));
      y = close(card, y + kSettingsRow);
    }

    y = Heading(l, L"Подключение", x0, x1, y);
    {
      const std::size_t card = open(y);
      float top = y;
      Item kill = CommandItem(Kind::Switch, row(top), L"Kill switch", nullptr, UiCommand::ToggleKillSwitch);
      kill.checked = c.killSwitch;
      kill.tick = kBucketBlock;
      l.items.push_back(std::move(kill));
      HintAfter(l, L"Kill switch", body_.get(), x0 + 20, x1 - 80, top + kSettingsRow / 2,
                L"Если подключение оборвётся, трафик не пойдёт мимо VPN: интернет закрыт, пока оно не "
                L"восстановится. Выключишь VPN сам — интернет работает как обычно.");
      top += kSettingsRow;
      line(top);
      Item startup = CommandItem(Kind::ValueRow, row(top), L"Автозапуск", nullptr, UiCommand::ChooseStartup);
      startup.detail = !c.autostart ? L"выключен" : c.connectAtStartup ? L"с подключением" : L"без подключения";
      startup.hinted = true;
      l.items.push_back(std::move(startup));
      HintAfter(l, L"Автозапуск", body_.get(), x0 + 20, x1 - 8, top + kSettingsRow / 2,
                L"При входе в Windows клиент запускается в области уведомлений, без окна. «С подключением» — "
                L"подключается сам, если был включён; «без подключения» — ждёт, пока включишь.");
      top += kSettingsRow;
      // The local network: open, or closed both ways but for what the user
      // lets in - the router, a device around, an address typed in. Closed, a
      // row per device let in (a click: shut again, LanChipMenu) and one to
      // let another in (LanAdd). Open, none: the service lets some in, it
      // doesn't shut some out.
      line(top);
      Item lan = CommandItem(Kind::Switch, row(top), L"Закрыть от локальной сети", nullptr, UiCommand::ToggleLanClosed);
      lan.checked = c.lanClosed;
      lan.tick = kBucketBlock;
      l.items.push_back(std::move(lan));
      HintAfter(l, L"Закрыть от локальной сети", body_.get(), x0 + 20, x1 - 80, top + kSettingsRow / 2,
                L"Фильтр только на этом компьютере. Роутер, телефоны и другие устройства работают как обычно и "
                L"пользуются интернетом — они просто не могут связаться с этим компьютером, а он с ними: общие "
                L"папки, принтер, трансляция на телевизор, передача файлов с телефона.\n\nИнтернет этого "
                L"компьютера тоже идёт через роутер, но зашифрованным: роутер не видит, какие сайты открыты, "
                L"кроме прямых. "
                L"Закрытый роутер — это только его настройки и его DNS.\n\nСовсем спрятаться нельзя: роутер "
                L"видит, что компьютер подключён, а сканер сети на телефоне — что по его адресу есть устройство. "
                L"Связаться с ним никто не сможет.\n\nДействует, пока подключение включено.");
      top += kSettingsRow;
      if (c.lanClosed) {
        for (std::size_t i = 0; i < c.lanHosts.size(); ++i) {
          const UiLanHost& h = c.lanHosts[i];
          if (!h.allowed) {
            continue;
          }
          line(top);
          const std::wstring title = h.router ? L"Роутер" : h.name.empty() ? h.address : h.name;
          Item host = CommandItem(Kind::ValueRow, row(top), title, nullptr, UiCommand::LanChipMenu,
                                  static_cast<int>(i));
          host.detail = !h.seen ? L"не в сети" : title != h.address ? h.address : L"";
          host.logo = "lan";
          l.items.push_back(std::move(host));
          top += kSettingsRow;
        }
        line(top);
        l.items.push_back(CommandItem(Kind::ValueRow, row(top), L"Разрешить устройство", kGlyphAdd, UiCommand::LanAdd));
        top += kSettingsRow;
      }
      y = close(card, top);
    }
    if (!c.killSwitchError.empty()) {
      y = Paragraph(l, Kind::ErrorText, L"Kill switch: " + c.killSwitchError, x0, x1, y) + kSettingsGap;
    }

    // What's cut on the way: the ads and the cookie windows. Switches, not a
    // way to somewhere - they were the routing board's block's chips till
    // 30.10.2026 (his: "не особо клеятся в блоке"). What was blocked lately
    // is a row under them: a click lets one through.
    y = Heading(l, L"Реклама", x0, x1, y);
    {
      const std::size_t card = open(y);
      float top = y;
      Item ads = CommandItem(Kind::Switch, row(top), L"Блокировать рекламу", nullptr, UiCommand::ToggleBlockAds);
      ads.checked = c.routing.blockAds;
      ads.tick = kBucketBlock;
      l.items.push_back(std::move(ads));
      HintAfter(l, L"Блокировать рекламу", body_.get(), x0 + 20, x1 - 80, top + kSettingsRow / 2,
                L"Запросы к рекламным и трекерным серверам по спискам никуда не уходят: страницы грузятся без "
                L"рекламы. Сломалось что-то нужное — «Недавно заблокировано» ниже: нажми имя, и оно пойдёт дальше.");
      top += kSettingsRow;
      line(top);
      Item cookies =
          CommandItem(Kind::Switch, row(top), L"Убирать баннеры cookie", nullptr, UiCommand::ToggleCookieWindows);
      cookies.checked = c.routing.cookieWindows;
      cookies.tick = kBucketBlock;
      l.items.push_back(std::move(cookies));
      HintAfter(l, L"Убирать баннеры cookie", body_.get(), x0 + 20, x1 - 80, top + kSettingsRow / 2,
                L"Убирает окна «принять cookie» OneTrust, Cookiebot и подобные. Своё окно сайта (TikTok, "
                L"DeepSeek) — нет: это не скрипт со стороны.");
      top += kSettingsRow;
      if (c.routing.blockAds || c.routing.cookieWindows) {
        line(top);
        Item recent = CommandItem(Kind::ValueRow, row(top), L"Недавно заблокировано", nullptr, UiCommand::BlockedMenu);
        recent.detail = L"пропустить нужное";
        l.items.push_back(std::move(recent));
        top += kSettingsRow;
      }
      y = close(card, top);
    }

    // The fine settings, folded: QUIC, IPv4, DNS - auto by default, seldom
    // touched (the routing page's till 17.10.2026).
    {
      // One card: the handle, its summary while folded, and - open - the seam and
      // the rows under it (the law's disclosure grammar).
      const std::size_t card = open(y);
      const float fold = FoldOf(c.routingMore, c.routingFold);
      Item more = CommandItem(Kind::ValueRow, row(y), L"Тонкая настройка", kGlyphChevronDown, UiCommand::ToggleRoutingMore);
      more.fold = fold;
      if (fold < 0.5f) {
        // What's set, not what's inside: QUIC as it stands, IPv4 only when on,
        // a DNS of the user's own when there is one.
        const UiRouting& r = c.routing;
        std::wstring set = r.quic == 1 ? L"QUIC выкл." : r.quic == 2 ? L"QUIC вкл." : L"";
        const auto add = [&](const wchar_t* part) { set += (set.empty() ? L"" : L" · ") + std::wstring(part); };
        if (r.ipv4Only) {
          add(L"IPv4");
        }
        if (!r.remoteDns.starts_with(L"авто") || !r.localDns.starts_with(L"авто")) {
          add(L"свой DNS");
        }
        more.detail = set.empty() ? L"QUIC, IPv4, DNS" : set;
      }
      l.items.push_back(std::move(more));
      float top = y + kSettingsRow;
      if (fold > 0) {
        const std::size_t first = l.items.size();
        top = FoldBody(l, first, top, FineRows(l, c, x0, x1, top), fold);
      }
      y = close(card, top);
    }

    // The foot, no card - it's about the app, nothing to set: the version and
    // whether it's the last, the data folder and the update check beside it;
    // an update waiting, its button under them. The sing-box inside a line of
    // its own, with whether it's sing-box's newest.
    // (The foot was loose lines and two icon buttons; since 30.10.2026, his:
    // "улучшить визуально и функциональнее", it's a card of rows with names:
    // updates and their check beside the versions they're about.)
    y = Heading(l, L"О Sovereign", x0, x1, y);
    {
      const std::size_t card = open(y);
      float top = y;
      const bool checking = c.update == UiUpdate::Checking || c.update == UiUpdate::Downloading;

      // The client's version and where it stands against the newest.
      Item self = Make(Kind::ValueRow, row(top), L"Sovereign");
      self.detail = c.version;
      if (c.update == UiUpdate::UpToDate) {
        self.detail += L" · последняя";
      } else if (c.update == UiUpdate::Available || c.update == UiUpdate::Waiting) {
        self.detail += L" · вышла " + c.updateVersion;
      }
      l.items.push_back(std::move(self));
      top += kSettingsRow;
      line(top);

      Item box = Make(Kind::ValueRow, row(top), L"sing-box");
      box.detail = c.boxVersion + (!c.boxIsLatest ? std::wstring()
                                   : *c.boxIsLatest ? std::wstring(L" · последняя")
                                                    : L" · вышла " + c.boxLatest);
      l.items.push_back(std::move(box));
      top += kSettingsRow;
      line(top);

      Item updates = CommandItem(Kind::ValueRow, row(top), L"Обновления", nullptr, UiCommand::ChooseUpdates);
      updates.detail = c.updateMode == UpdateMode::Off       ? L"не проверять"
                       : c.updateMode == UpdateMode::Install ? L"ставить сами"
                                                             : L"только проверять";
      updates.hinted = true;
      l.items.push_back(std::move(updates));
      HintAfter(l, L"Обновления", body_.get(), x0 + 20, x1 - 8, top + kSettingsRow / 2,
                L"«Только проверять» — раз в 12 часов; новая версия появится значком в заголовке окна, "
                L"обновляешь нажатием на него. «Ставить сами» — новая версия скачивается сама и "
                L"ставится без вопросов, когда компьютер свободен: никто не трогал мышь и клавиатуру столько минут, сколько выбрано строкой ниже, "
                L"ничего не открыто на весь экран, ничего не качается. VPN пропадает примерно на полминуты. «Не "
                L"проверять» — только строкой «Проверить сейчас» ниже.");
      top += kSettingsRow;
      line(top);

      if (c.updateMode == UpdateMode::Install) {  // how long the computer must be left alone before it installs
        Item quiet = CommandItem(Kind::ValueRow, row(top), L"Ставить после тишины", nullptr, UiCommand::ChooseUpdateQuiet);
        quiet.detail = std::to_wstring(c.updateQuietMinutes) + L" мин";
        l.items.push_back(std::move(quiet));
        top += kSettingsRow;
        line(top);
      }

      // The check by hand; its answer in the row.
      Item check = CommandItem(Kind::ValueRow, row(top), L"Проверить сейчас", kGlyphRefresh, UiCommand::CheckUpdate);
      check.enabled = !checking && c.update != UiUpdate::Off;
      check.detail = c.update == UiUpdate::Checking      ? L"проверяю…"
                     : c.update == UiUpdate::Downloading ? L"скачиваю " + c.updateVersion + L"…"
                     : c.update == UiUpdate::UpToDate    ? L"это последняя версия"
                     : c.update == UiUpdate::Waiting     ? L"скачана, ждёт паузы"
                     : c.update == UiUpdate::Off         ? L"не в этой сборке"
                     : c.update == UiUpdate::Failed || (c.update == UiUpdate::Available && !c.updateError.empty())
                         ? L"не вышло"
                         : L"";
      l.items.push_back(std::move(check));
      top += kSettingsRow;
      line(top);

      Item folder = CommandItem(Kind::ValueRow, row(top), L"Папка данных", kGlyphFolder, UiCommand::OpenFolder);
      folder.tip = L"Открыть папку данных: настройки, конфиги, их история";
      l.items.push_back(std::move(folder));
      top += kSettingsRow;
      y = close(card, top);
    }
    if (c.update == UiUpdate::Failed || (c.update == UiUpdate::Available && !c.updateError.empty())) {
      y = Paragraph(l, Kind::ErrorText, UpdateStatus(c), x0, x1, y) + kSettingsGap;
    }
    if (c.update == UiUpdate::Available || c.update == UiUpdate::Waiting) {
      y += 10;
      l.items.push_back(CommandItem(Kind::AccentButton, {x0, y, x1, y + kButton},
                                    c.update == UiUpdate::Waiting ? L"Обновить сейчас до " + c.updateVersion
                                                                  : L"Обновить до " + c.updateVersion,
                                    kGlyphDownload, UiCommand::InstallUpdate));
      y += kButton;
    }
    return y;
  }

  /* ---- drawing ---- */

  void DrawItem(const Canvas& k, const Item& it, const UiContent& c, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    const D2D1_COLOR_F primary = FromColorRef(ui::kPrimaryText);
    const D2D1_COLOR_F secondary = FromColorRef(ui::kSecondaryText);
    const D2D1_COLOR_F lit = FromColorRef(ui::kLit);
    const float hoverAlpha = pressed ? 0.04f : (hovered ? 0.08f : 0.0f);
    switch (it.kind) {
      case Kind::Card:
        // Glass: a breath of white over what's behind, a brighter edge; else the card's own colour.
        if (!it.quiet) {
          k.Round(r, kRadius, it.glass ? Rgb(255, 255, 255, 0.035f) : FromColorRef(ui::kCardColor));
          if (hoverAlpha > 0 && it.action != ItemAction::None) {  // a card that is a link
            k.Round(r, kRadius, Rgb(255, 255, 255, hoverAlpha));
          }
          k.Outline(r, kRadius, Rgb(255, 255, 255, it.glass ? 0.10f : 0.08f));
        }
        if (it.rail >= 0) {  // a way's card: the settings' card, and the rail down its edge - no tint, no other geometry
          // An empty way: the same rail, short and at full tone (a pale one fails 3:1), at the head's middle.
          const float mid = (r.top + r.bottom) / 2;
          const float top = it.quiet ? mid - kRailQuiet / 2 : r.top + 8;
          const float bottom = it.quiet ? mid + kRailQuiet / 2 : r.bottom - 8;
          k.Round({r.left, top, r.left + 3, bottom}, 1.5f, FromColorRef(BucketColor(it.rail), kRailAlpha));
        }
        break;
      case Kind::Divider:  // a groove cut in the panel: dark, and the lit lip under it
        k.Line({r.left, r.top + 0.5f}, {r.right, r.top + 0.5f}, Rgb(0, 0, 0, 0.35f));
        k.Line({r.left, r.top + 1.5f}, {r.right, r.top + 1.5f}, Rgb(255, 255, 255, 0.05f));
        break;
      case Kind::Title: k.Text(it.text, title_.get(), r, primary); break;
      case Kind::Heading: k.Text(it.text, heading_.get(), r, primary); break;
      case Kind::Text: k.Text(it.text, body_.get(), r, it.warn ? FromColorRef(ui::kDanger) : primary); break;
      case Kind::Muted:  // one that picks something (a Command): brighter under the mouse
        k.Text(it.engraved ? Spaced(it.text) : it.text, it.engraved ? engraved_.get() : caption_.get(), r,
               it.warn ? FromColorRef(ui::kDanger)
                       : (hovered && it.action == ItemAction::Command ? primary : secondary));
        break;
      case Kind::Meter: {
        // A thin track and how much of it is used: amber from three
        // quarters, red from nine tenths.
        const float share = std::clamp(static_cast<float>(it.index) / 1000, 0.0f, 1.0f);
        k.Round(r, 1.5f, Rgb(255, 255, 255, 0.08f));
        const COLORREF used = share >= 0.9f ? ui::kDanger : share >= 0.75f ? ui::kWarning : ui::kPrimaryText;
        k.Round({r.left, r.top, r.left + (r.right - r.left) * share, r.bottom}, 1.5f,
                FromColorRef(used, used == ui::kPrimaryText ? 0.75f : 1.0f));
        break;
      }
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
      case Kind::SpeedGraph: DrawSpeed(k, it, c); break;
      case Kind::Question: DrawQuestion(k, c); break;
      case Kind::Day: DrawDay(k, it, hovered, pressed); break;
      case Kind::ServerCard: {
        if (hoverAlpha > 0 && it.enabled) {
          k.Round(r, kRadius - 1, Rgb(255, 255, 255, hoverAlpha));
        }
        const float mid = (r.top + r.bottom) / 2;
        k.Text(it.text, heading_.get(), {r.left + 16, r.top + 6, r.right - 34, mid + 2},
               it.enabled ? primary : secondary);
        k.Text(it.detail, caption_.get(), {r.left + 16, mid + 2, r.right - 34, r.bottom - 6}, secondary);
        if (it.enabled) {
          k.Text(it.glyph, glyph_.get(), {r.right - 30, r.top, r.right - 6, r.bottom}, secondary);
        }
        break;
      }
      case Kind::ValueRow: {
        if (hoverAlpha > 0 && it.enabled) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const float cy = (r.top + r.bottom) / 2;
        DrawTick(k, it, cy);
        float textLeft = r.left + 16;
        if (ID2D1Bitmap* logo = it.logo.empty() ? nullptr : Logo(k.t, it.logo); logo != nullptr) {
          k.t->DrawBitmap(logo, {textLeft, cy - 8, textLeft + 16, cy + 8});
          textLeft += 22;
        }
        // The value starts after the title and the "?" that follows it.
        const float valueLeft = textLeft + TextWidth(it.text, body_.get()) + 12 + (it.hinted ? kHint + 6 : 0.0f);
        // `warn`: a row that ends something (quit) - in red, its glyph too.
        const D2D1_COLOR_F ink = it.warn ? FromColorRef(ui::kDanger) : it.enabled ? primary : secondary;
        // A row that only tells (a version): its value runs to the edge, no arrow.
        const bool tells = it.action == ItemAction::None && it.glyph.empty();
        k.Text(it.text, body_.get(), {textLeft, r.top, valueLeft, r.bottom}, ink);
        k.Text(it.detail, trailing_.get(),
               {valueLeft, r.top, r.right - (tells ? 16.0f : 30.0f), r.bottom}, secondary);
        // Its own glyph if it has one (a fold's), else "goes on to a choice".
        if (!tells) {
          DrawIcon(k, it.glyph.empty() ? std::wstring_view(kGlyphChevron) : std::wstring_view(it.glyph),
                   {r.right - 28, r.top, r.right - 8, r.bottom}, it.warn ? ink : secondary, std::max(it.fold, 0.0f));
        }
        break;
      }
      case Kind::Banner: {
        // The hand's plate, lit .08, for every banner. Checked: what's wrong (Overview's Issues) - the
        // warning's dot stands where the glyph would, the plate stays; the verdict is never a fill or a glyph.
        k.Round(r, kRadius, FromColorRef(ui::kLit, pressed ? 0.05f : (hovered ? 0.12f : 0.08f)));
        const D2D1_COLOR_F ink = it.enabled ? primary : secondary;
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse({r.left + 24, (r.top + r.bottom) / 2}, 4.5f, 4.5f),
                           k.Color(FromColorRef(ui::kWarning)));
        } else {
          k.Text(it.glyph, glyph_.get(), {r.left + 10, r.top, r.left + 38, r.bottom}, ink);
        }
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
        if (it.key) {
          DrawKey(k, it, hovered && it.enabled, pressed && it.enabled);
          break;
        }
        if (it.chrome) {
          DrawChrome(k, it, hovered && it.enabled, pressed && it.enabled);
          break;
        }
        if (hoverAlpha > 0 && it.enabled) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        DrawIcon(k, it.glyph, r, !it.enabled ? FromColorRef(ui::kSecondaryText, 0.4f) : (hovered ? primary : secondary));
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse({r.right - 9, r.top + 9}, 4, 4), k.Color(lit));
        }
        break;
      case Kind::Switch: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const float right = r.right - 16;
        const D2D1_COLOR_F ink = it.enabled ? primary : secondary;
        DrawTick(k, it, (r.top + r.bottom) / 2);
        if (it.detail.empty()) {
          k.Text(it.text, body_.get(), {r.left + 16, r.top, right - 60, r.bottom}, ink);
        } else {
          k.Text(it.text, body_.get(), {r.left + 16, r.top + 10, right - 60, r.top + 34}, ink);
          k.Text(it.detail, caption_.get(), {r.left + 16, r.top + 34, right - 60, r.top + 56}, secondary);
        }
        DrawSwitch(k, {right - 40, (r.top + r.bottom) / 2 - 10, right, (r.top + r.bottom) / 2 + 10}, it.checked);
        break;
      }
      case Kind::Choice: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const D2D1_POINT_2F dot{r.left + 22, (r.top + r.bottom) / 2};
        if (it.checked) {
          k.t->FillEllipse(D2D1::Ellipse(dot, 9, 9), k.Color(lit));
          k.t->FillEllipse(D2D1::Ellipse(dot, 4, 4), k.Color(FromColorRef(ui::kLitInk)));
        } else {
          k.t->DrawEllipse(D2D1::Ellipse(dot, 8.5f, 8.5f), k.Color(secondary), kIconStroke);
        }
        // With a detail: the name and the latency on top, the detail under them.
        const float mid = (r.top + r.bottom) / 2;
        const D2D1_RECT_F line = it.detail.empty() ? r : D2D1_RECT_F{r.left, r.top + 4, r.right, mid + 2};
        // Which sites open through the server, right to left before the latency:
        // green opens, red answers "not from here", dim doesn't answer.
        float nameRight = line.right - 120;
        if (it.command == UiCommand::SetProtocol && it.index >= 0 && static_cast<std::size_t>(it.index) < c.sites.size()) {
          const auto& sites = c.sites[static_cast<std::size_t>(it.index)];
          for (auto site = sites.rbegin(); site != sites.rend(); ++site) {
            const float width = TextWidth(site->label, caption_.get()) + 6;
            const D2D1_COLOR_F ink = site->state == SiteCheck::State::Ok        ? FromColorRef(ui::kUpload)
                                     : site->state == SiteCheck::State::Blocked ? FromColorRef(ui::kDanger)
                                                                                : secondary;
            k.Text(site->label, caption_.get(), {nameRight - width, line.top, nameRight, line.bottom}, ink);
            nameRight -= width + 6;
          }
        }
        k.Text(it.text, body_.get(), {line.left + 42, line.top, nameRight - 8, line.bottom}, primary);
        if (!it.detail.empty()) {
          k.Text(it.detail, caption_.get(), {r.left + 42, mid + 2, r.right - 12, r.bottom - 4}, secondary);
        }
        if (it.command == UiCommand::SetProtocol && it.index >= 0 && static_cast<std::size_t>(it.index) < c.delays.size()) {
          const UiDelay& delay = c.delays[static_cast<std::size_t>(it.index)];
          k.Text(DelayLabel(delay, c.delaysDetailed), delay_.get(), {line.right - 120, line.top, line.right - 12, line.bottom},
                 FromColorRef(ui::DelayColor(delay)));
        }
        break;
      }
      case Kind::ProfileRow: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const float mid = (r.top + r.bottom) / 2;
        k.Text(it.text, body_.get(), {r.left + 14, r.top + 2, r.right - 28, mid + 1}, it.checked ? primary : secondary);
        k.Text(it.detail, caption_.get(), {r.left + 14, mid + 1, r.right - 28, r.bottom - 2},
               it.warn ? FromColorRef(ui::kDanger) : secondary);
        k.Text(it.glyph, glyph_.get(), {r.right - 26, r.top, r.right - 4, r.bottom}, secondary);
        break;
      }
      case Kind::CheckRow: {
        const std::array<D2D1_COLOR_F, 5> colors = {secondary, lit, FromColorRef(ui::kUpload),
                                                    FromColorRef(ui::kWarning), FromColorRef(ui::kDanger)};
        const D2D1_COLOR_F status = colors[static_cast<std::size_t>(std::clamp(it.index, 0, 4))];
        const float cy = (r.top + r.bottom) / 2;
        k.t->FillEllipse(D2D1::Ellipse({r.left + 20.5f, cy}, 4.5f, 4.5f), k.Color(status));  // its ink from the line 20
        // The title and its "?" (a Hint item after it), the result in what's
        // left on the right - cut with "…": the "?" has it in full.
        const float titleRight = r.left + 34 + TextWidth(it.text, body_.get());
        k.Text(it.text, body_.get(), {r.left + 34, r.top, titleRight + 2, r.bottom}, primary);
        const float resultLeft = titleRight + 6 + kHint + 8;
        const float resultRight = r.right - CheckButtonRoom(it);
        const bool quiet = it.index <= 1;  // not run, running
        k.Text(it.detail, trailing_.get(), {resultLeft, r.top, resultRight, r.bottom},
               quiet ? secondary : (it.index >= 3 ? status : primary));
        break;
      }
      case Kind::Hint: {
        const D2D1_POINT_2F center{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        const D2D1_COLOR_F ink = hovered ? primary : FromColorRef(ui::kSecondaryText, 0.85f);
        k.t->DrawEllipse(D2D1::Ellipse(center, 7.5f, 7.5f), k.Color(ink), kIconStroke);
        k.Text(L"?", hint_.get(), {center.x - 8, center.y - 8.5f, center.x + 8, center.y + 7.5f}, ink);
        break;
      }
      case Kind::Toggle: {
        if (hoverAlpha > 0) {
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
        }
        const float cx = (r.left + r.right) / 2;
        const float cy = (r.top + r.bottom) / 2;
        DrawSwitch(k, {cx - 20, cy - 10, cx + 20, cy + 10}, it.checked);
        break;
      }
      case Kind::Segment: {
        // Picked: a key pressed in and lit - the light's white, not the VPN's
        // blue (ui_style.h); the other a groove, its word dimmer.
        if (it.checked) {
          k.Round(r, 6, FromColorRef(ui::kLit, pressed ? 0.08f : (hovered ? 0.16f : 0.12f)));
          k.Outline(r, 6, FromColorRef(ui::kLit, 0.40f));
        } else {
          k.Round(r, 6, Rgb(0, 0, 0, 0.18f));
          k.Round(r, 6, Rgb(255, 255, 255, hoverAlpha));
          k.Outline(r, 6, Rgb(255, 255, 255, 0.08f));
        }
        k.Text(it.text, button_.get(), r, it.checked ? primary : (hovered ? primary : secondary));
        break;
      }
      case Kind::Chip:
        if (it.checked) {
          k.Round(r, 15, FromColorRef(ui::kLit, pressed ? 0.08f : (hovered ? 0.16f : 0.12f)));
          k.Outline(r, 15, FromColorRef(ui::kLit, 0.40f));
        } else {
          k.Round(r, 15, Rgb(255, 255, 255, hoverAlpha));
          k.Outline(r, 15, Rgb(255, 255, 255, 0.10f));
        }
        k.Text(it.text, chip_.get(), r, it.checked ? primary : secondary);
        break;
      case Kind::Pill: {
        // A window cut into the panel: dark, a hairline, a shadow along its top edge.
        if (it.rail >= 0) {  // a way's label: tinted in its colour, a dot before the name
          const COLORREF tone = BucketColor(it.rail);
          const float cy = (r.top + r.bottom) / 2;
          k.Round(r, (r.bottom - r.top) / 2, FromColorRef(tone, 0.12f));
          k.Outline(r, (r.bottom - r.top) / 2, FromColorRef(tone, 0.40f));
          k.t->FillEllipse(D2D1::Ellipse({r.left + 11, cy}, 3, 3), k.Color(FromColorRef(tone)));
          k.Text(it.text, caption_.get(), {r.left + 22, r.top, r.right, r.bottom}, primary);
          break;
        }
        // `warn`: the verdict's lamp - glow and rim in the warning's hue, the word stays white.
        const D2D1_COLOR_F ink = it.warn ? primary : secondary;
        k.Round(r, 5, Rgb(0, 0, 0, 0.30f));
        if (it.warn) {
          k.Round(r, 5, FromColorRef(ui::kWarning, 0.12f));
        }
        k.Outline(r, 5, it.warn ? FromColorRef(ui::kWarning, 0.40f) : Rgb(255, 255, 255, 0.08f));
        k.Text(it.text, caption_.get(), {r.left + kPillPad, r.top, r.right, r.bottom}, ink);
        break;
      }
      case Kind::AppTitle: {
        const float cy = (r.top + r.bottom) / 2;
        float left = r.left;
        if (ID2D1Bitmap* icon = ProgramIcon(k.t, it.text2); icon != nullptr) {
          k.t->DrawBitmap(icon, {left, cy - 8, left + 16, cy + 8});
          left += 24;
        }
        k.Text(it.text, caption_.get(), {left, r.top, r.right, r.bottom}, secondary);
        break;
      }
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
      case Kind::Tag: {
        k.Round(r, 8, Rgb(255, 255, 255, 0.05f + hoverAlpha));
        k.Outline(r, 8, Rgb(255, 255, 255, 0.08f));
        const float cy = (r.top + r.bottom) / 2;
        float left = r.left + 12;  // the law's chip: 12 inside, 8 between its parts
        if (ID2D1Bitmap* icon = it.logo.empty() ? ProgramIcon(k.t, it.text2) : Logo(k.t, it.logo); icon != nullptr) {
          k.t->DrawBitmap(icon, {left, cy - 8, left + 16, cy + 8});
          left += 24;
        }
        // A thing that goes here anyway (the rest's way, UiChip::idle) is set quieter: named, no exception.
        k.Text(it.text, body_.get(), {left, r.top, r.right - 10, r.bottom}, it.quiet ? secondary : primary);  // 2 of slack: see Board
        if (!it.detail.empty()) {
          const float after = left + std::ceil(TextWidth(it.text, body_.get())) + 8;
          k.Text(it.detail, caption_.get(), {after, r.top, r.right - 10, r.bottom}, secondary);
        }
        break;
      }
      case Kind::LogBox: break;  // DrawLog
    }
  }

  // A tip over everything, as AIControls has it (24.10.2026, he: "аккуратненько,
  // как в айконтролс"): a dark box hugging its words, a nose pointing at the
  // middle of what it's about - over it, under it where the window ends
  // above (the title bar's keys), inside the window. A "?"'s paragraphs wrap
  // and get more room round them.
  void DrawTip(const Canvas& k, const Item& it, D2D1_SIZE_F size) const {
    const std::wstring_view text = TipText(it);
    if (text.empty()) {
      return;
    }
    constexpr float kNose = 6;     // the triangle's height, and half its base
    constexpr float kTipGap = 6;   // between its point and the item
    constexpr float kTipRound = 6;
    constexpr float kEdge = 6;     // the window's edge it keeps off
    // A "?"'s paragraphs narrow enough to read; a key's line rather unbroken.
    const float maxWidth = std::min(it.kind == Kind::Hint ? 300.0f : 360.0f, size.width - 2 * kEdge - 20);
    const auto m = Metrics(text, tip_.get(), maxWidth, 10000);
    if (!m) {
      return;
    }
    const bool wrapped = m->lineCount > 1;
    const float padX = wrapped ? 10.0f : 8.0f;
    const float padY = wrapped ? 8.0f : 4.0f;
    const float textWidth = std::min(maxWidth, std::ceil(m->widthIncludingTrailingWhitespace) + 1);
    const float w = textWidth + 2 * padX;
    const float h = std::ceil(m->height) + 2 * padY;

    // What it points at: a key's pill, a title's words - not the cell, the row.
    D2D1_RECT_F at = it.chrome ? KeyPill(it) : it.rect;
    if (it.kind == Kind::Title) {
      at.right = std::min(at.right, at.left + TextWidth(it.text, title_.get()));
    }
    const float middle = (at.left + at.right) / 2;
    const float left = std::clamp(middle - w / 2, kEdge, std::max(kEdge, size.width - kEdge - w));
    const float above = at.top - kTipGap - kNose - kEdge;
    const float under = size.height - kEdge - (at.bottom + kTipGap + kNose);
    const bool below = h > above && (h <= under || under > above);
    const float top = below ? std::min(at.bottom + kTipGap + kNose, std::max(kEdge, size.height - kEdge - h))
                            : std::max(kEdge, at.top - kTipGap - kNose - h);

    const D2D1_COLOR_F fill = FromColorRef(ui::kRaisedColor);
    k.Round({left, top, left + w, top + h}, kTipRound, fill);
    const float noseX = std::clamp(middle, left + kTipRound + kNose, left + w - kTipRound - kNose);
    const float baseY = below ? top + 0.5f : top + h - 0.5f;  // a hair into the box: no seam
    const float pointY = below ? top - kNose : top + h + kNose;
    wil::com_ptr<ID2D1PathGeometry> nose;
    wil::com_ptr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(d2d_->CreatePathGeometry(nose.put())) && SUCCEEDED(nose->Open(sink.put()))) {
      sink->BeginFigure({noseX - kNose, baseY}, D2D1_FIGURE_BEGIN_FILLED);
      sink->AddLine({noseX, pointY});
      sink->AddLine({noseX + kNose, baseY});
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      if (SUCCEEDED(sink->Close())) {
        k.t->FillGeometry(nose.get(), k.Color(fill));
      }
    }
    k.Text(text, tip_.get(), {left + padX, top + padY, left + padX + textWidth, top + h - padY + 2},
           FromColorRef(ui::kPrimaryText));
  }

  static D2D1_COLOR_F ToneColor(UiQuestion::Tone tone, float alpha = 1) {
    switch (tone) {
      case UiQuestion::Tone::Warning: return FromColorRef(ui::kWarning, alpha);
      case UiQuestion::Tone::Danger: return FromColorRef(ui::kDanger, alpha);
      case UiQuestion::Tone::Accent: break;
    }
    return FromColorRef(ui::kLit, alpha);
  }

  // MainWindow::Ask's card (AskBox): a soft shadow under it, lighter than
  // the cards of the page, its edge catching the light at the top, the
  // buttons' band darker below a line.
  void DrawQuestion(const Canvas& k, const UiContent& c) const {
    if (!c.question) {
      return;
    }
    const UiQuestion& q = *c.question;
    const D2D1_SIZE_F size = k.t->GetSize();
    const AskBoxes b = AskBox(q, size.width, size.height);
    const D2D1_RECT_F r = b.card;
    constexpr float kAskRadius = 12;
    for (int i = 6; i >= 1; --i) {
      const auto f = static_cast<float>(i);
      k.Round({r.left - f * 2.5f, r.top - f * 1.5f, r.right + f * 2.5f, r.bottom + f * 4}, kAskRadius + f * 2.5f,
              Rgb(0, 0, 0, 0.07f));
    }
    k.Round(r, kAskRadius, FromColorRef(ui::kCardColor));  // the band's
    k.Round({r.left, r.top, r.right, b.band}, kAskRadius, FromColorRef(ui::kDialogColor));
    k.Fill({r.left, b.band - kAskRadius, r.right, b.band}, FromColorRef(ui::kDialogColor));
    k.Line({r.left, b.band + 0.5f}, {r.right, b.band + 0.5f}, Rgb(255, 255, 255, 0.06f));
    k.Outline(r, kAskRadius, Rgb(255, 255, 255, 0.09f));
    k.Line({r.left + kAskRadius, r.top + 1.5f}, {r.right - kAskRadius, r.top + 1.5f}, Rgb(255, 255, 255, 0.07f));

    const D2D1_POINT_2F mid{(b.circle.left + b.circle.right) / 2, (b.circle.top + b.circle.bottom) / 2};
    const float radius = (b.circle.right - b.circle.left) / 2;
    k.t->FillEllipse(D2D1::Ellipse(mid, radius, radius), k.Color(ToneColor(q.tone, 0.15f)));
    k.t->DrawEllipse(D2D1::Ellipse(mid, radius - 0.5f, radius - 0.5f), k.Color(ToneColor(q.tone, 0.28f)), 1);
    k.Text(q.glyph.empty() ? std::wstring_view(kGlyphWarning) : std::wstring_view(q.glyph), glyph_.get(),
           b.circle, ToneColor(q.tone));
    k.Text(q.title, ask_.get(), b.title, FromColorRef(ui::kPrimaryText));
    k.Text(q.text, wrap_.get(), b.text, FromColorRef(ui::kSecondaryText));
    if (q.calendar) {
      // The month and the year between their steps (the keys are items, DrawDay);
      // the weekdays over the grid, a hairline under them.
      const UiCalendar& cal = *q.calendar;
      const CalendarKeys keys = CalendarHead(b.calHead);
      k.Text(kMonthNames.at(static_cast<std::size_t>(std::clamp(cal.month, 1, 12) - 1)), center_.get(), keys.monthName,
             FromColorRef(ui::kPrimaryText));
      k.Text(std::to_wstring(cal.year), center_.get(), keys.yearName, FromColorRef(ui::kPrimaryText));
      const float width = (b.calGrid.right - b.calGrid.left) / 7;
      for (std::size_t i = 0; i < kWeekdayNames.size(); ++i) {
        const float left = b.calGrid.left + width * static_cast<float>(i);
        k.Text(kWeekdayNames[i], hint_.get(), {left, b.calWeek.top, left + width, b.calWeek.bottom},
               FromColorRef(ui::kSecondaryText, i >= 5 ? 0.6f : 1.0f));
      }
      const float line = std::floor(b.calGrid.top) - 1.5f;
      k.Line({b.calGrid.left, line}, {b.calGrid.right, line}, Rgb(255, 255, 255, 0.07f));
    }
  }

  // A calendar key on a question's card: a step (a chevron) or a day.
  void DrawDay(const Canvas& k, const Item& it, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    if (!it.glyph.empty()) {
      if (hovered || pressed) {
        k.Round(r, 6, Rgb(255, 255, 255, pressed ? 0.05f : 0.10f));
      }
      const float cx = std::floor((r.left + r.right) / 2) + 0.5f;
      const float cy = std::floor((r.top + r.bottom) / 2) + 0.5f;
      const float direction = static_cast<float>(it.sub);
      const D2D1_COLOR_F ink = FromColorRef(hovered ? ui::kPrimaryText : ui::kSecondaryText);
      k.t->DrawLine({cx - direction * 2, cy - 4}, {cx + direction * 2, cy}, k.Color(ink), 1.3f, roundCaps_.get());
      k.t->DrawLine({cx + direction * 2, cy}, {cx - direction * 2, cy + 4}, k.Color(ink), 1.3f, roundCaps_.get());
      return;
    }
    D2D1_COLOR_F ink = FromColorRef(ui::kPrimaryText);
    if (it.checked) {
      k.Round(r, 6, FromColorRef(ui::kLit, pressed ? 0.8f : 1.0f));
      ink = FromColorRef(ui::kLitInk);
    } else {
      if (hovered || pressed) {
        k.Round(r, 6, Rgb(255, 255, 255, pressed ? 0.05f : 0.10f));
      }
      if (it.sub == 1) {  // today
        k.Outline(r, 6, FromColorRef(ui::kLit, 0.45f));
      }
    }
    k.Text(it.text, button_.get(), r, ink);
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
    const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor(log.scroll / kLogLine)));
    for (auto i = static_cast<int>(first); i < log.Count(); ++i) {
      const float top = box.top + static_cast<float>(i) * kLogLine - log.scroll;
      if (top > box.bottom) {
        break;
      }
      const D2D1_RECT_F row{box.left, top, box.right, top + kLogLine};
      if (log.Selected(i)) {
        k.Fill(row, FromColorRef(ui::kLit, 0.12f));
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
          levelColor = FromColorRef(ui::kWarning);
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
    if (const auto thumb = LogThumb(box, log)) {
      // Thin at rest; wider and brighter under the pointer, to be taken.
      const float width = log.barHot ? 8.0f : 3.0f;
      k.Round({box.right - 4 - width, thumb->top + 2, box.right - 4, thumb->bottom - 2}, width / 2,
              Rgb(255, 255, 255, log.barHot ? 0.45f : 0.22f));
    }
    if (log.pan) {
      // Where the middle button went down: the farther from it, the faster.
      const D2D1_POINT_2F at = *log.pan;
      k.t->FillEllipse(D2D1::Ellipse(at, 12, 12), k.Color(Rgb(20, 24, 32, 0.85f)));
      k.t->DrawEllipse(D2D1::Ellipse(at, 12, 12), k.Color(Rgb(255, 255, 255, 0.6f)), 1.2f);
      k.t->FillEllipse(D2D1::Ellipse(at, 2.5f, 2.5f), k.Color(Rgb(255, 255, 255, 0.85f)));
    }
    k.t->PopAxisAlignedClip();
  }

  void DrawSwitch(const Canvas& k, D2D1_RECT_F track, bool on) const {
    const float mid = (track.top + track.bottom) / 2;
    if (on) {
      k.Round(track, 10, FromColorRef(ui::kLit));
      k.t->FillEllipse(D2D1::Ellipse({track.right - 10, mid}, 6, 6), k.Color(FromColorRef(ui::kLitInk)));
    } else {
      k.Outline(track, 10, FromColorRef(ui::kSecondaryText), kIconStroke);
      k.t->FillEllipse(D2D1::Ellipse({track.left + 10, mid}, 5, 5), k.Color(FromColorRef(ui::kSecondaryText)));
    }
  }

  // The fate's mark on the card's edge (3x12, the rail's tone): a row whose fate is known puts it
  // at its middle - while its switch is on, or always for a way (a DNS). The row sits 4 in from
  // the card's edge, so the mark stands on the edge itself. The switch stays white.
  void DrawTick(const Canvas& k, const Item& it, float cy) const {
    if (it.tick < 0 || !(it.tickAlways || it.checked)) {
      return;
    }
    const float x = it.rect.left - 4;
    k.Round({x, cy - 6, x + 3, cy + 6}, 1.5f, FromColorRef(BucketColor(it.tick), kRailAlpha));
  }

  void DrawButton(const Canvas& k, const Item& it, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    D2D1_COLOR_F fill = Rgb(255, 255, 255, 0.06f);
    D2D1_COLOR_F ink = FromColorRef(ui::kPrimaryText);
    if (it.kind == Kind::AccentButton) {
      fill = FromColorRef(ui::kLit, pressed ? 0.8f : (hovered ? 0.9f : 1.0f));
      ink = FromColorRef(ui::kLitInk);
    } else if (it.kind == Kind::DangerButton) {
      // The block's lamp: a glow and a rim, the word stays white - no red fill (law.md, Button).
      fill = FromColorRef(ui::kDanger, pressed ? 0.08f : (hovered ? 0.18f : 0.12f));
    } else if (hovered) {
      fill = Rgb(255, 255, 255, pressed ? 0.05f : 0.10f);
    }
    if (it.action == ItemAction::Answer && it.index == 1) {
      // A question's yes is always the hand's: white. The tone burns as the lamp in the question's head.
      fill = FromColorRef(ui::kLit, pressed ? 0.8f : (hovered ? 0.9f : 1.0f));
      ink = FromColorRef(ui::kLitInk);
    }
    const bool lamp = it.kind == Kind::DangerButton && !(it.action == ItemAction::Answer && it.index == 1);
    if (!it.enabled) {
      fill = Rgb(255, 255, 255, 0.03f);
      ink = FromColorRef(ui::kSecondaryText, 0.5f);
    }
    k.Round(r, 6, fill);
    if (lamp && it.enabled) {
      k.Outline(r, 6, FromColorRef(ui::kDanger, 0.40f));
    } else if (it.kind == Kind::Button || lamp) {
      k.Outline(r, 6, Rgb(255, 255, 255, 0.08f));
    }
    // Glyph and text centered together.
    float textWidth = 0;
    if (const auto m = Metrics(it.text, button_.get(), r.right - r.left, r.bottom - r.top)) {
      textWidth = m->width;
    }
    const float glyphWidth = it.glyph.empty() ? 0.0f : 26.0f;
    const float left = (r.left + r.right - textWidth - glyphWidth) / 2;
    if (!it.glyph.empty()) {
      k.Text(it.glyph, glyph_.get(), {left, r.top, left + 18, r.bottom}, ink);
    }
    const D2D1_RECT_F textBox{left + glyphWidth, r.top, left + glyphWidth + textWidth + 2, r.bottom};
    k.Text(it.text, body_.get(), textBox, ink);
  }

  // The live traffic's dots round the switch's square `cube`: each with a
  // soft halo and a short trail behind it, fading in and out along its path;
  // sparks fade as they fly.
  // The speed test's graph: one timeline as the test runs - the download
  // (kSpeedDownSeconds) in the VPN's blue, then the upload
  // (kSpeedUpSeconds) in green after it - each smoothed over three samples,
  // growing from the left; the scale rounded up, lines at its top and half;
  // the speeds on top: the mean past the first second, as the test's result.
  // (Both laid over the whole width at once read as one thing - 16.10.2026.)
  void DrawSpeed(const Canvas& k, const Item& it, const UiContent& c) const {
    if (it.index < 0 || static_cast<std::size_t>(it.index) >= c.checks.size()) {
      return;
    }
    const UiCheck& check = c.checks[static_cast<std::size_t>(it.index)];
    const D2D1_RECT_F r = it.rect;
    const D2D1_COLOR_F down = FromColorRef(ui::kVpn);
    const D2D1_COLOR_F up = FromColorRef(ui::kUpload);
    const D2D1_COLOR_F secondary = FromColorRef(ui::kSecondaryText);
    const auto smooth = [](const std::vector<float>& s) {
      std::vector<float> out(s.size());
      for (std::size_t i = 0; i < s.size(); ++i) {
        const std::size_t lo = i > 1 ? i - 2 : 0;
        const std::size_t hi = std::min(i + 2, s.empty() ? 0 : s.size() - 1);
        float sum = 0;
        for (std::size_t j = lo; j <= hi; ++j) {
          sum += s[j];
        }
        out[i] = sum / static_cast<float>(hi - lo + 1);
      }
      return out;
    };
    const std::vector<float> downs = smooth(check.speedDown);
    const std::vector<float> ups = smooth(check.speedUp);
    float peak = 1;
    for (const auto* s : {&downs, &ups}) {
      for (const float v : *s) {
        peak = std::max(peak, v);
      }
    }
    static constexpr std::array<float, 14> kScales = {10, 20, 50, 100, 150, 200, 300, 500, 750, 1000, 1500, 2000, 5000, 10000};
    float scale = kScales.back();
    for (const float s : kScales) {
      if (s >= peak * 1.1f) {
        scale = s;
        break;
      }
    }

    const float pad = 16;
    const float legend = r.top + 10;
    // Each one as wide as its text: boxes of a fixed width left the units
    // "Мбит/с · чере…" on a narrow window (24.10.2026).
    float x = r.left + pad;
    const auto speed = [&](const std::wstring& text, D2D1_COLOR_F color) {
      const float width = TextWidth(text, heading_.get());
      k.Text(text, heading_.get(), {x, legend, x + width + 2, legend + 22}, color);
      x += width + 14;
    };
    speed(std::format(L"↓ {:.0f}", StreamMbps(check.speedDown)), down);
    if (!check.speedUp.empty()) {
      speed(std::format(L"↑ {:.0f}", StreamMbps(check.speedUp)), up);
    }
    const std::wstring scaleText = std::format(L"{:.0f}", scale);
    const float scaleLeft = r.right - pad - TextWidth(scaleText, trailing_.get()) - 2;
    // Which way it measured, said with the units: in "only what's chosen" the
    // tray's own requests go directly - the line, not the VPN (its detail
    // starts so: diagnose.cpp Speed).
    const bool direct = check.detail.starts_with(L"Напрямую");
    k.Text(direct ? L"Мбит/с · напрямую" : L"Мбит/с · через VPN", caption_.get(),
           {x, legend + 3, scaleLeft - 8, legend + 22}, secondary);
    k.Text(scaleText, trailing_.get(), {scaleLeft, legend + 3, r.right - pad, legend + 22}, secondary);

    const D2D1_RECT_F plot{r.left + pad, r.top + 42, r.right - pad, r.bottom - 14};
    const float mid = (plot.top + plot.bottom) / 2;
    k.Line({plot.left, plot.top}, {plot.right, plot.top}, Rgb(255, 255, 255, 0.05f));
    k.Line({plot.left, mid}, {plot.right, mid}, Rgb(255, 255, 255, 0.05f));
    k.Line({plot.left, plot.bottom}, {plot.right, plot.bottom}, Rgb(255, 255, 255, 0.12f));

    const float perSecond = 1000.0f / static_cast<float>(kSpeedSampleMs);
    const float downSlots = static_cast<float>(kSpeedDownSeconds) * perSecond;
    const float allSlots = downSlots + static_cast<float>(kSpeedUpSeconds) * perSecond;
    const float dx = (plot.right - plot.left) / std::max(1.0f, allSlots - 1);
    // Where the download hands over to the upload.
    if (!ups.empty()) {
      const float seam = plot.left + (downSlots - 0.5f) * dx;
      k.Line({seam, plot.top}, {seam, plot.bottom}, Rgb(255, 255, 255, 0.06f));
    }
    // `growing`: the series the test is on - its newest point drawn only
    // speedTip of the way from the one before, eased there frame by frame.
    const auto series = [&](const std::vector<float>& s, float firstSlot, D2D1_COLOR_F color, bool growing) {
      if (s.size() < 2) {
        return;
      }
      std::vector<D2D1_POINT_2F> points;
      points.reserve(s.size());
      for (std::size_t i = 0; i < s.size(); ++i) {
        const float x = std::min(plot.right, plot.left + (firstSlot + static_cast<float>(i)) * dx);
        const float y = plot.bottom - std::min(s[i] / scale, 1.0f) * (plot.bottom - plot.top);
        points.push_back({x, y});
      }
      if (growing) {
        const D2D1_POINT_2F from = points[points.size() - 2];
        D2D1_POINT_2F& to = points.back();
        const float t = std::clamp(speedTip, 0.0f, 1.0f);
        to = {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
      }
      // A curve through the points' midpoints (each point its control): no corners, as speed tests draw.
      const auto trace = [&](ID2D1GeometrySink* sink) {
        for (std::size_t i = 1; i + 1 < points.size(); ++i) {
          const D2D1_POINT_2F mid{(points[i].x + points[i + 1].x) / 2, (points[i].y + points[i + 1].y) / 2};
          sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(points[i], mid));
        }
        sink->AddLine(points.back());
      };
      wil::com_ptr<ID2D1PathGeometry> area;
      wil::com_ptr<ID2D1GeometrySink> sink;
      if (SUCCEEDED(d2d_->CreatePathGeometry(area.put())) && SUCCEEDED(area->Open(sink.put()))) {
        sink->BeginFigure({points.front().x, plot.bottom}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(points.front());
        trace(sink.get());
        sink->AddLine({points.back().x, plot.bottom});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (SUCCEEDED(sink->Close())) {
          D2D1_COLOR_F fill = color;
          fill.a = 0.12f;
          k.t->FillGeometry(area.get(), k.Color(fill));
        }
      }
      wil::com_ptr<ID2D1PathGeometry> line;
      wil::com_ptr<ID2D1GeometrySink> lineSink;
      if (SUCCEEDED(d2d_->CreatePathGeometry(line.put())) && SUCCEEDED(line->Open(lineSink.put()))) {
        lineSink->BeginFigure(points.front(), D2D1_FIGURE_BEGIN_HOLLOW);
        trace(lineSink.get());
        lineSink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (SUCCEEDED(lineSink->Close())) {
          k.t->DrawGeometry(line.get(), k.Color(color), 2);
        }
      }
    };
    series(downs, 0, down, ups.empty());
    series(ups, downSlots, up, !ups.empty());
  }

  void DrawLive(const Canvas& k, const LiveTraffic& live, D2D1_RECT_F cube) const {
    for (const Particle& p : live.particles) {
      D2D1_COLOR_F color = FromColorRef(ui::kVpn);
      float alpha = std::sin(std::numbers::pi_v<float> * std::clamp(p.t, 0.0f, 1.0f));
      switch (p.stream) {
        case Stream::Vpn: break;
        case Stream::Warp: color = FromColorRef(ui::kWarp); break;
        case Stream::Direct: color = FromColorRef(ui::kDirect); alpha *= 0.7f; break;
        case Stream::Blocked:
          color = FromColorRef(ui::kDanger);
          alpha = std::min(1.0f, p.t * 4);
          break;
        case Stream::Spark:
          color = FromColorRef(ui::kDanger);
          alpha = 1 - p.t / kSparkLife;
          break;
      }
      if (alpha <= 0.01f) {
        continue;
      }
      const D2D1_POINT_2F at = DotAt(p, cube);
      // The trail: where it was a moment ago, fainter.
      if (p.stream != Stream::Spark) {
        for (int i = 1; i <= 3; ++i) {
          Particle back = p;
          back.t -= 0.018f * static_cast<float>(i);
          if (back.t > 0) {
            const D2D1_POINT_2F was = DotAt(back, cube);
            color.a = alpha * 0.45f / static_cast<float>(i);
            k.t->FillEllipse(D2D1::Ellipse(was, p.size * 0.8f, p.size * 0.8f), k.Color(color));
          }
        }
      }
      color.a = alpha * 0.16f;
      k.t->FillEllipse(D2D1::Ellipse(at, p.size * 2.8f, p.size * 2.8f), k.Color(color));
      color.a = alpha * 0.95f;
      k.t->FillEllipse(D2D1::Ellipse(at, p.size, p.size), k.Color(color));
    }
  }

  // The main screen's backdrop: the dark, and a light from behind the switch
  // (`at`, its rect) - the VPN's blue while connected, a faint grey otherwise.
  // A stage for the live traffic to come.
  void DrawBackdrop(ID2D1RenderTarget* t, D2D1_SIZE_F size, D2D1_RECT_F at, bool on) const {
    const D2D1_POINT_2F center{(at.left + at.right) / 2, (at.top + at.bottom) / 2};
    const float radius = std::max(size.width, at.bottom - at.top) * 0.75f;
    const D2D1_COLOR_F light = on ? FromColorRef(ui::kVpn, 0.22f) : Rgb(255, 255, 255, 0.05f);
    const std::array<D2D1_GRADIENT_STOP, 3> stops{
        {{0, light}, {0.45f, D2D1_COLOR_F{light.r, light.g, light.b, light.a * 0.35f}}, {1, D2D1_COLOR_F{light.r, light.g, light.b, 0}}}};
    wil::com_ptr<ID2D1GradientStopCollection> collection;
    wil::com_ptr<ID2D1RadialGradientBrush> brush;
    if (SUCCEEDED(t->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put())) &&
        SUCCEEDED(t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center, {0, 0}, radius, radius),
                                               collection.get(), brush.put()))) {
      t->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), brush.get());
    }
  }
  // An icon button of the main screen's glass card: as flat as the title bar's
  // (a ring round the plus looked like a button sewn on - he, 22.10.2026) - the
  // glyph alone, a soft square under it with the pointer. Its text is
  // grayscale: ClearType on the translucent card fringes the glyph red and blue.
  void DrawKey(const Canvas& k, const Item& it, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    if (hovered || pressed) {
      k.Round(r, 8, Rgb(255, 255, 255, pressed ? 0.14f : 0.09f));
    }
    const D2D1_TEXT_ANTIALIAS_MODE mode = k.t->GetTextAntialiasMode();
    k.t->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    k.Text(it.glyph, glyph_.get(), r, Rgb(242, 244, 248, hovered ? 1.0f : 0.8f));
    k.t->SetTextAntialiasMode(mode);
  }
  // A title bar key's pill: inset from the bar's top and the window's edge,
  // not the whole cell up to them.
  // A key's pill; a new version's is thinner (30.10.2026, his: the button was
  // big, its top edge flush with the window's), with its number on it as wide
  // as its cell.
  static D2D1_RECT_F KeyPill(const Item& it) {
    if (it.checked) {
      const float cy = (it.rect.top + it.rect.bottom) / 2;
      if (it.detail.empty()) {
        const float cx = (it.rect.left + it.rect.right) / 2;
        return {cx - 16, cy - 10, cx + 16, cy + 10};
      }
      return {it.rect.left + 2, cy - 10, it.rect.right - 2, cy + 10};
    }
    return ChromePill(it.rect);
  }
  // Capitals cut into a panel breathe: a hair space between the letters (a
  // text format has no letter spacing; only a layout has).
  static std::wstring Spaced(std::wstring_view s) {
    std::wstring out;
    for (const wchar_t ch : s) {
      if (!out.empty() && ch != L' ' && out.back() != L' ') {
        out += L'\x200A';
      }
      out += ch;
    }
    return out;
  }
  // The title bar's own icons, drawn as lines like the cross and the dash -
  // one stroke, round ends, 14 DIPs across, nothing filled - instead of
  // Segoe's glyphs, which differ in weight and detail from each other
  // (30.10.2026, his: "слишком пестро ... я хотел что ты сам рисунок кнопок
  // переделал"). False: not one of ours, the font's glyph is drawn.
  // A glyph in `box`: ours drawn in lines where there is one, else the font's.
  // `turn`: a fold's chevron, 0 pointing down (folded) .. 1 up (open), turned half a circle by it.
  void DrawIcon(const Canvas& k, std::wstring_view glyph, D2D1_RECT_F box, D2D1_COLOR_F ink, float turn = 0) const {
    // Through pixels' middles: at 100 % lines a pixel thick, not two halves.
    const float cx = std::floor((box.left + box.right) / 2) + 0.5f;
    const float cy = std::floor((box.top + box.bottom) / 2) + 0.5f;
    if (!DrawLineIcon(k, glyph, cx, cy, ink, turn)) {
      k.Text(glyph, glyph_.get(), box, ink);
    }
  }
  bool DrawLineIcon(const Canvas& k, std::wstring_view glyph, float cx, float cy, D2D1_COLOR_F ink,
                    float turn = 0) const {
    const auto line = [&](float x0, float y0, float x1, float y1) {
      k.t->DrawLine({cx + x0, cy + y0}, {cx + x1, cy + y1}, k.Color(ink), kIconStroke, roundCaps_.get());
    };
    const auto ring = [&](float x, float y, float rx, float ry) {
      k.t->DrawEllipse(D2D1::Ellipse({cx + x, cy + y}, rx, ry), k.Color(ink), kIconStroke);
    };
    if (glyph == kGlyphSettings) {
      // Three sliders: a line each, a ring on it where it's set.
      const std::array<std::pair<float, float>, 3> sliders{{{-4.5f, -2.5f}, {0.0f, 2.5f}, {4.5f, -0.5f}}};
      for (const auto& [y, x] : sliders) {
        line(-7, y, x - 2.9f, y);
        line(x + 2.9f, y, 7, y);
        ring(x, y, 1.9f, 1.9f);
      }
    } else if (glyph == kGlyphLog) {
      // A sheet with its lines.
      k.t->DrawRoundedRectangle(D2D1::RoundedRect({cx - 5.5f, cy - 7, cx + 5.5f, cy + 7}, 2, 2), k.Color(ink),
                                kIconStroke);
      line(-2.5f, -2.5f, 2.5f, -2.5f);
      line(-2.5f, 0.5f, 2.5f, 0.5f);
      line(-2.5f, 3.5f, 0.5f, 3.5f);
    } else if (glyph == kGlyphRoute) {
      // A way from one point to another: a step up, with a ring at its start and a dot at its end.
      ring(-5, 5, 1.8f, 1.8f);
      line(-5, 3.2f, -5, -0.5f);
      line(-5, -0.5f, 5, -0.5f);
      line(5, -0.5f, 5, -3.2f);
      k.t->FillEllipse(D2D1::Ellipse({cx + 5, cy - 5}, 1.8f, 1.8f), k.Color(ink));
    } else if (glyph == kGlyphReport) {
      // A bug: the body split down its back, a head, legs and feelers.
      ring(0, 1.5f, 3.4f, 4.4f);
      line(0, -2.9f, 0, 5.9f);
      line(-1.4f, -4.2f, -2.8f, -6.2f);
      line(1.4f, -4.2f, 2.8f, -6.2f);
      for (const float y : {-0.8f, 2.0f, 4.6f}) {
        line(-3.4f, y, -6.4f, y - 1.0f);
        line(3.4f, y, 6.4f, y - 1.0f);
      }
    } else if (glyph == kGlyphChevron) {  // goes on to a choice
      line(-1.8f, -4, 2.2f, 0);
      line(2.2f, 0, -1.8f, 4);
    } else if (glyph == kGlyphChevronDown || glyph == kGlyphChevronUp) {  // a fold: opens, closes
      // Down (folded) turned half a circle round its middle to up (open) by `turn`.
      const float angle = (glyph == kGlyphChevronUp ? 1.0f : turn) * 3.14159265f;
      const float sine = std::sin(angle);
      const float cosine = std::cos(angle);
      const auto at = [&](float x, float y) { return D2D1_POINT_2F{x * cosine - y * sine, x * sine + y * cosine}; };
      const D2D1_POINT_2F a = at(-4, -2);
      const D2D1_POINT_2F b = at(0, 2);
      const D2D1_POINT_2F c = at(4, -2);
      line(a.x, a.y, b.x, b.y);
      line(b.x, b.y, c.x, c.y);
    } else if (glyph == kGlyphAdd) {
      line(-5.5f, 0, 5.5f, 0);
      line(0, -5.5f, 0, 5.5f);
    } else if (glyph == kGlyphRefresh) {
      // Three quarters of a ring and its arrowhead at the open end, up right.
      wil::com_ptr<ID2D1PathGeometry> arc;
      wil::com_ptr<ID2D1GeometrySink> sink;
      constexpr float kR = 5.5f;
      if (SUCCEEDED(d2d_->CreatePathGeometry(arc.put())) && SUCCEEDED(arc->Open(sink.put()))) {
        sink->BeginFigure({cx + kR, cy}, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment({cx, cy - kR}, {kR, kR}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (SUCCEEDED(sink->Close())) {
          k.t->DrawGeometry(arc.get(), k.Color(ink), kIconStroke, roundCaps_.get());
        }
      }
      line(0, -kR, -2.6f, -kR - 2.4f);
      line(0, -kR, -2.6f, -kR + 2.4f);
    } else if (glyph == kGlyphFolder) {
      // Its outline: the tab up left, the body.
      const std::array<D2D1_POINT_2F, 7> outline{
          {{-7, -4.5f}, {-2.2f, -4.5f}, {-0.6f, -2.8f}, {7, -2.8f}, {7, 5}, {-7, 5}, {-7, -4.5f}}};
      for (std::size_t i = 1; i < outline.size(); ++i) {
        line(outline[i - 1].x, outline[i - 1].y, outline[i].x, outline[i].y);
      }
    } else {
      return false;
    }
    return true;
  }
  static D2D1_RECT_F ChromePill(D2D1_RECT_F r) {
    const float cx = (r.left + r.right) / 2;
    const float cy = (r.top + r.bottom) / 2;
    return {cx - 16, cy - 14, cx + 16, cy + 14};
  }
  // A key of the title bar (Caption): the pill under the pointer; the cross
  // and the dash drawn as lines of Windows' own size - ChromeClose at the
  // glyphs' 16 DIPs was the biggest thing in the bar, "it doesn't fit in"
  // (24.10.2026). The cross's pill is a soft red, the cross on it red.
  void DrawChrome(const Canvas& k, const Item& it, bool hovered, bool pressed) const {
    const D2D1_RECT_F r = it.rect;
    const bool cross = it.glyph == kGlyphClose;
    if (!it.checked && (hovered || pressed)) {
      k.Round(ChromePill(r), 8,
              cross ? FromColorRef(ui::kDanger, pressed ? 0.14f : 0.22f)
                    : Rgb(255, 255, 255, pressed ? 0.05f : 0.08f));
    }
    // Checked (a new version's key, Caption): a solid accent pill with a
    // halo, its number on it where it fits - the one thing on the bar that's
    // news (30.10.2026, his: "ахуенный значок"; it was the arrow in the
    // accent alone, lost among the keys).
    if (it.checked) {
      // An indicator window: a dark recess with a lit hairline and a pilot
      // lamp lit in it, the version in white (30.10.2026: the solid blue pill
      // was a button; this is a reading). White since: blue is the VPN's.
      const D2D1_RECT_F pill = KeyPill(it);
      const D2D1_COLOR_F lamp = FromColorRef(ui::kLit);
      k.Round(pill, 7, Rgb(0, 0, 0, 0.38f));
      k.Round(pill, 7, FromColorRef(ui::kLit, pressed ? 0.03f : (hovered ? 0.10f : 0.05f)));
      k.Outline(pill, 7, FromColorRef(ui::kLit, hovered ? 0.60f : 0.35f));
      if (it.detail.empty()) {
        k.Text(it.glyph, glyph_.get(), r, FromColorRef(ui::kPrimaryText));
        k.Led({pill.right - 5, pill.top + 5}, 2.0f, lamp);
      } else {
        k.Led({pill.left + 12, (pill.top + pill.bottom) / 2}, 2.5f, lamp);
        k.Text(it.detail, pill_.get(), {pill.left + 22, r.top, pill.right, r.bottom}, FromColorRef(ui::kPrimaryText));
      }
      return;
    }
    const D2D1_COLOR_F ink = !it.enabled ? FromColorRef(ui::kSecondaryText, 0.4f)
                             : !hovered   ? FromColorRef(ui::kSecondaryText)
                             : cross      ? FromColorRef(ui::kDanger)
                                          : FromColorRef(ui::kPrimaryText);
    // Through pixels' middles: at 100 % lines a pixel thick, not two halves.
    const float cx = std::floor((r.left + r.right) / 2) + 0.5f;
    const float cy = std::floor((r.top + r.bottom) / 2) + 0.5f;
    constexpr float kHalf = 4.5f;  // with the round ends 10 DIPs across
    if (cross) {
      k.t->DrawLine({cx - kHalf, cy - kHalf}, {cx + kHalf, cy + kHalf}, k.Color(ink), kIconStroke, roundCaps_.get());
      k.t->DrawLine({cx - kHalf, cy + kHalf}, {cx + kHalf, cy - kHalf}, k.Color(ink), kIconStroke, roundCaps_.get());
    } else if (it.glyph == kGlyphMinimize) {
      k.t->DrawLine({cx - kHalf, cy}, {cx + kHalf, cy}, k.Color(ink), kIconStroke, roundCaps_.get());
    } else if (!DrawLineIcon(k, it.glyph, cx, cy, ink)) {
      k.Text(it.glyph, glyph_.get(), r, ink);
    }
  }
  // The switch is a cube (cube.h): in the pose the window animates it
  // through, or - a still picture, a snapshot - the state's own.
  void DrawPower(const Canvas& k, const Item& it, const UiContent& c, bool hovered, bool pressed) const {
    DrawCube(k.t, k.cube != nullptr ? *k.cube : CubePoseFor(c.display), it.rect, hovered, pressed);
  }

  wil::com_ptr<ID2D1Factory> d2d_;
  wil::com_ptr<IDWriteFactory> dwrite_;
  wil::com_ptr<IDWriteFontCollection> fonts_;  // Geist; null if missing
  wil::com_ptr<IWICImagingFactory> wic_;
  wil::com_ptr<ID2D1StrokeStyle> roundCaps_;  // the title bar's cross and dash
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
  wil::com_ptr<IDWriteTextFormat> trailing_;  // a result on a row's right, cut with "…"
  wil::com_ptr<IDWriteTextFormat> hint_;      // the "?" of a hint
  wil::com_ptr<IDWriteTextFormat> pill_;      // a new version's number on its pill in the title bar
  wil::com_ptr<IDWriteTextFormat> engraved_;  // a panel's label: small capitals, spaced
  wil::com_ptr<IDWriteTextFormat> tip_;       // a tip's text, wrapped
  wil::com_ptr<IDWriteTextFormat> ask_;       // a question's title, wrapped
  wil::com_ptr<IDWriteTextFormat> glyph_;
  // ProgramIcon's, by exe path; tied to one render target (ForgetIcons).
  mutable std::map<std::wstring, wil::com_ptr<ID2D1Bitmap>> icons_;
  mutable std::map<std::string, wil::com_ptr<ID2D1Bitmap>> logos_;  // Logo's, by service; ForgetIcons too
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
  bool settingsMore = false;   // the configurations' relay fold open (UiContent::settingsMore)
  // The servers' order on their page (UiContent::serverOrder): kept through
  // a latency test - every figure "…" there, the list would fall back to the
  // subscription's order and jump again as the results came in.
  std::vector<std::size_t> serverOrder;
  // MainWindow::Ask's question while it's open (UiContent::question), what
  // its yes does, and when it came (Painter::questionShown).
  std::optional<UiQuestion> question;
  std::function<void()> onYes;
  std::function<void()> onOther;
  std::function<void(const UiCalendar&)> onDate;  // a calendar's yes (AskDate)
  std::chrono::steady_clock::time_point questionAt{};
  // When the pointer's wait for a tip is over. The timer alone isn't enough:
  // while the live traffic keeps a WM_PAINT waiting, WM_TIMER is never
  // handed out (a paint outranks it) and no tip showed on the main screen
  // (30.10.2026, he: 'при наводке на таракана подсказку бы'). The paint
  // looks at the clock too.
  std::optional<std::chrono::steady_clock::time_point> tipDue;
  std::optional<float> barGrab;  // dragging the log's thumb: where in it it was taken, DIPs below its top
  ULONGLONG panSince = 0;        // the middle button went down (log.pan)
  bool panMoved = false;         // the pointer left the still zone since
  bool panEndedByRight = false;  // a right press ended it: no menu on its release
  std::optional<POINT> mouse;  // the cursor over the client area, in pixels
  bool placed = false;  // sized and put by the tray once, on the first show

  wil::com_ptr<ID2D1HwndRenderTarget> target;
  ULONGLONG deactivatedAt = 0;  // GetTickCount64 when the window last lost the focus

  // The connect cube (cube.h): from the pose it was in to its state's since
  // cubeStart, turning while it connects. A timer only while it moves and
  // the window shows.
  CubePose cubeFrom;
  CubePose cubeTo;
  ULONGLONG cubeStart = 0;
  Display cubeDisplay = Display::Starting;
  bool cubeKnown = false;

  // Windows' "animation effects" (Accessibility > Visual effects): off, the
  // cube takes each pose at once.
  static bool Motion() {
    BOOL on = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) == FALSE || on != FALSE;
  }

  // Where the turn has got to, without the idle life - what a new turn starts from.
  CubePose CubeBase(ULONGLONG now) const {
    if (!Motion()) {
      return cubeTo;
    }
    const auto since = static_cast<float>(now - cubeStart);
    CubePose pose = BlendPose(cubeFrom, cubeTo, EaseOut(since / static_cast<float>(kCubeTurnMs)));
    if (cubeDisplay == Display::Starting) {
      pose.yaw = cubeFrom.yaw + since * kCubeSpin;
    }
    return pose;
  }

  // What's drawn: the turn, and over it the small idle life (Idle).
  CubePose CubeAt(ULONGLONG now) const {
    const CubePose base = CubeBase(now);
    return Motion() ? Idle(base, cubeDisplay, static_cast<double>(now) / 1000.0) : base;
  }

  bool CubeMoving(ULONGLONG now) const {
    return Motion() && (cubeDisplay == Display::Starting || now - cubeStart < kCubeTurnMs);
  }

  // Alive at rest too (Idle): while the overview shows the cube, on screen.
  bool CubeIdling() const {
    return Motion() && cubeKnown && cubeDisplay != Display::ServiceDown && page == UiPage::Overview &&
           hwnd != nullptr && IsWindowVisible(hwnd) != FALSE && IsIconic(hwnd) == FALSE && !Blocked();
  }


  // A new state: a turn from wherever the cube is into its pose - forward,
  // to the next square-on yaw (askew by the pose's own offset).
  void CubeTo(Display display) {
    const ULONGLONG now = GetTickCount64();
    if (cubeKnown && display == cubeDisplay) {
      return;
    }
    CubePose to = CubePoseFor(display);
    if (!cubeKnown) {
      cubeKnown = true;
      cubeFrom = to;
      cubeStart = display == Display::Starting ? now : now - kCubeTurnMs;
    } else {
      cubeFrom = CubeBase(now);
      cubeFrom.yaw = std::fmod(cubeFrom.yaw, 360.0f);
      to.yaw = NextRestYaw(cubeFrom.yaw) + (to.yaw - 45);
      cubeStart = now;
    }
    cubeTo = to;
    cubeDisplay = display;
    KickCube();
  }

  void KickCube() {
    if (hwnd != nullptr && IsWindowVisible(hwnd) != FALSE && (CubeMoving(GetTickCount64()) || CubeIdling())) {
      SetTimer(hwnd, kCubeTimer, CubeMoving(GetTickCount64()) ? kCubeFrameMs : kCubeIdleFrameMs, nullptr);
    }
  }

  // The live traffic (LiveTraffic): its timer runs while the overview shows
  // and there's traffic to draw or dots still on their way; it stops itself.
  LiveTraffic live;
  // Frames as often as the screen shows them - 60, 144, whatever it runs at:
  // each paint ends waiting for the screen's refresh (the render target's
  // EndDraw) and asks for the next one, so no timer (Windows' can't go past
  // 64 a second, and its 15.6 ms ticks made the dots jerk). The last step,
  // by the precise clock; empty: not running.
  std::optional<std::chrono::steady_clock::time_point> liveAt;
  // The speed test's graph easing to its newest sample (NoteSpeed): how many it had, and when the last came.
  std::size_t speedSamples = 0;
  std::chrono::steady_clock::time_point speedSampleAt{};

  // New speed samples in the content: the graph's tip starts again from the
  // one before (Painter::speedTip), frames asked for while it eases.
  void NoteSpeed() {
    std::size_t samples = 0;
    if (content.checks.size() > kCheckSpeed) {
      samples = content.checks[kCheckSpeed].speedDown.size() + content.checks[kCheckSpeed].speedUp.size();
    }
    const bool more = samples > speedSamples;
    speedSamples = samples;
    if (more && Motion()) {
      speedSampleAt = std::chrono::steady_clock::now();
      painter.speedTip = 0;
      if (hwnd != nullptr && IsWindowVisible(hwnd) != FALSE && page == UiPage::Checks) {
        InvalidateRect(hwnd, nullptr, FALSE);
      }
    }
  }

  // A frame of the speed test's graph: its tip eased towards the newest
  // sample over one sample's time; true while there's way to go.
  bool StepSpeed() {
    if (page != UiPage::Checks || IsWindowVisible(hwnd) == FALSE || !Motion() || Blocked()) {
      painter.speedTip = 1;
      return false;
    }
    const float t = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - speedSampleAt).count() /
                    static_cast<float>(kSpeedSampleMs);
    painter.speedTip = std::min(1.0f, t);
    return t < 1.0f;
  }

  // A fold opening or closing (the fine settings', the relay's): how open it is, eased over
  // kFoldMs from where it was; its flag in the content says where it goes.
  struct FoldMove {
    bool known = false;
    float from = 0;
    float to = 0;
    std::chrono::steady_clock::time_point at{};
    float Progress() const {
      const float t = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - at).count() /
                      static_cast<float>(kFoldMs);
      return std::clamp(t, 0.0f, 1.0f);
    }
    float Now() const { return from + (to - from) * EaseOut(Progress()); }
    bool Moving() const { return known && from != to && Progress() < 1.0f; }
  };
  FoldMove routingMove;
  FoldMove relayMove;

  // The flag says where the fold goes; if that's new, it moves from where it is - at once with the
  // window away, a modal box up or Windows' animations off.
  void Aim(FoldMove& move, bool open) {
    const float to = open ? 1.0f : 0.0f;
    const bool still = !Motion() || hwnd == nullptr || IsWindowVisible(hwnd) == FALSE || Blocked();
    if (!move.known || still) {
      move = {true, to, to, std::chrono::steady_clock::now()};
    } else if (move.to != to) {
      move = {true, move.Now(), to, std::chrono::steady_clock::now()};
    }
  }

  // The content's folds as they stand now: at rest -1, the flag says all.
  void SetFolds() {
    content.routingFold = routingMove.Moving() ? routingMove.Now() : -1.0f;
    content.relayFold = relayMove.Moving() ? relayMove.Now() : -1.0f;
  }

  // New flags (from the host, or the window's own): the folds aimed anew.
  void NoteFolds() {
    Aim(routingMove, content.routingMore);
    Aim(relayMove, settingsMore);
    SetFolds();
  }

  // A frame of a fold moving: the page laid out again at its height of now; true while one still does.
  bool StepFold() {
    if (!routingMove.Moving() && !relayMove.Moving()) {
      if (content.routingFold >= 0 || content.relayFold >= 0) {  // the last frame: at rest
        SetFolds();
        Relayout();
      }
      return false;
    }
    SetFolds();
    Relayout();
    return true;
  }

  // A frame of a question coming in (Painter::questionShown); true while it still is.
  bool StepQuestion() {
    if (!question || IsWindowVisible(hwnd) == FALSE || !Motion() || Blocked()) {
      painter.questionShown = 1;
      return false;
    }
    const float t = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - questionAt).count() /
                    static_cast<float>(kQuestionInMs);
    painter.questionShown = std::min(1.0f, t);
    return t < 1.0f;
  }

  // A modal box over the window (MessageBox, a file dialog) disables it. Such
  // a box shows itself only once its loop finds the queue empty, and a frame
  // asking for the next one keeps a paint there: the box stayed invisible and
  // the disabled window looked hung (22.10.2026, the cross's question). No
  // frames are asked for meanwhile; WM_ENABLE starts them again.
  bool Blocked() const { return IsWindowEnabled(hwnd) == FALSE; }

  void KickLive() {
    if (!liveAt && hwnd != nullptr && IsWindowVisible(hwnd) != FALSE && page == UiPage::Overview && Motion() &&
        (content.display == Display::On || !live.particles.empty())) {
      liveAt = std::chrono::steady_clock::now();
      InvalidateRect(hwnd, nullptr, FALSE);
    }
  }

  // A step before a paint: whether to go on (the next frame asked for after it).
  bool StepLiveTraffic() {
    auto now = std::chrono::steady_clock::now();
    if (liveAt && now - *liveAt < std::chrono::milliseconds(2)) {
      // Faster than any screen: nothing waited for its refresh (a remote
      // session, say) - wait for the compositor's, not spin.
      DwmFlush();
      now = std::chrono::steady_clock::now();
    }
    const float dt = liveAt ? std::min(0.1f, std::chrono::duration<float>(now - *liveAt).count()) : 0.0f;
    liveAt = now;
    const bool on = content.display == Display::On;
    if (!layout.glow || page != UiPage::Overview || IsWindowVisible(hwnd) == FALSE || !Motion() || Blocked() ||
        (!on && live.particles.empty())) {
      liveAt.reset();
      if (page != UiPage::Overview || IsWindowVisible(hwnd) == FALSE) {
        live.particles.clear();  // shown again, it starts afresh
      }
      return false;
    }
    StepLive(live, content, on, dt, *layout.glow);
    return true;
  }

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
    // WS_CLIPCHILDREN: a repaint (a hover anywhere repaints it all) leaves an
    // edit box in place over its row alone - drawn over, it flickered as the
    // mouse moved (15.10.2026).
    // No minimize or maximize: a window of the tray's - the cross hides it
    // there; its edges still size it (a long log).
    window.reset(CreateWindowExW(0, kClassName, L"Sovereign",
                                 (WS_OVERLAPPEDWINDOW & ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, this));
    THROW_LAST_ERROR_IF(!window);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    // The frame again, now with the title bar ours (WM_NCCALCSIZE).
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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

  // The item under a point: the last laid out wins - drawn last, it's on top
  // (a "?" over its row, a button in a check's line).
  int HitAt(POINT px, bool anyItem = false) const {
    const float x = ToDip(px.x);
    const float y = ToDip(px.y);
    for (std::size_t i = layout.items.size(); i-- > 0;) {
      const Item& item = layout.items[i];
      if (question && item.action != ItemAction::Answer) {
        continue;  // a question open: the page under it takes nothing
      }
      if ((!anyItem && !Interactive(item)) || !Contains(Seen(item), x, y)) {
        continue;
      }
      if (item.scrolls && (y < layout.viewTop || y >= layout.viewBottom)) {
        continue;
      }
      return static_cast<int>(i);
    }
    return -1;
  }

  // What shows a tip: a "?" at once, an icon button (its name) after a while.
  bool IsKind(int index, Kind kind) const {
    return index >= 0 && static_cast<std::size_t>(index) < layout.items.size() &&
           layout.items[static_cast<std::size_t>(index)].kind == kind;
  }

  // Shows its words after a while: an icon button its name, anything with a
  // tip of its own that. A "?" isn't one: its shows at once.
  bool HasTip(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < layout.items.size() && !IsKind(index, Kind::Hint) &&
           !TipText(layout.items[static_cast<std::size_t>(index)]).empty();
  }

  // The pointer is over `h` now: a "?" shows its tip, anything else with one
  // (HasTip) starts the wait for it - none if it comes straight from another
  // tip (along the title bar's keys: the names one after another).
  void Hover(int h) {
    if (h == in.hover) {
      return;
    }
    const bool tipShown = in.tip >= 0 && !IsKind(in.tip, Kind::Hint);
    in.hover = h;
    KillTimer(hwnd, kTipTimer);
    tipDue.reset();
    in.tip = IsKind(h, Kind::Hint) || (tipShown && HasTip(h)) ? h : -1;
    if (in.tip < 0 && HasTip(h)) {
      SetTimer(hwnd, kTipTimer, kTipDelayMs, nullptr);
      tipDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTipDelayMs);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
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

  // On the log's scrollbar strip, with something to scroll.
  bool OnLogBar(POINT px) const {
    if (!layout.logBox || !InLog(px)) {
      return false;
    }
    return LogThumb(*layout.logBox, log) && ToDip(px.x) >= layout.logBox->right - kLogBar;
  }

  // The thumb's top dragged to `top` (DIPs): the lines scrolled to match.
  void DragLogThumb(float top) {
    if (!layout.logBox) {
      return;
    }
    const D2D1_RECT_F box = *layout.logBox;
    const auto thumb = LogThumb(box, log);
    if (!thumb) {
      return;
    }
    const float view = box.bottom - box.top;
    const float room = view - (thumb->bottom - thumb->top);
    const float at = room > 0 ? std::clamp((top - box.top) / room, 0.0f, 1.0f) : 0.0f;
    ScrollLog(at * (log.Height() - view) - log.scroll);
  }

  static HCURSOR PanCursor() { return LoadCursorW(nullptr, MAKEINTRESOURCEW(32645)); }  // IDC_SIZENS

  // The middle button's scrolling, as browsers have it: held and moved, it
  // ends on release; clicked, it goes on until the next press, a key or the wheel.
  void StartPan(POINT px) {
    SetCapture(hwnd);  // first: taken from another, WM_CAPTURECHANGED would end it
    log.pan = D2D1::Point2F(ToDip(px.x), ToDip(px.y));
    panSince = GetTickCount64();
    panMoved = false;
    SetTimer(hwnd, kPanTimer, kPanFrameMs, nullptr);
    SetCursor(PanCursor());
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  void StopPan() {
    if (!log.pan) {
      return;
    }
    log.pan.reset();
    KillTimer(hwnd, kPanTimer);
    if (GetCapture() == hwnd) {
      ReleaseCapture();
    }
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  // A frame of it: the farther the pointer from where it was pressed, the
  // faster - a few lines a second near, thousands far.
  void PanStep() {
    POINT px{};
    GetCursorPos(&px);
    ScreenToClient(hwnd, &px);
    if (!log.pan) {
      return;
    }
    const float off = ToDip(px.y) - log.pan->y;
    const float beyond = std::abs(off) - kPanStill;
    if (beyond <= 0) {
      return;
    }
    panMoved = true;
    ScrollLog(std::copysign(beyond * 0.12f + beyond * beyond * 0.003f, off));
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
    content.settingsMore = settingsMore;
    content.serverOrder = serverOrder;
    content.question = question;
    layout = painter.Build(content, page, openProfile, size.width, size.height, scroll, state);
    ClampLog();
    // The same spot may hold another item now (the content refreshes every
    // second under a resting pointer): it is pointed at anew, its tip timed
    // as if the pointer had just come; the same item keeps its tip.
    const int over = mouse ? HitAt(*mouse) : -1;
    if (over != in.hover) {
      Hover(over);
    } else {
      in.tip = IsKind(over, Kind::Hint) || (in.tip >= 0 && HasTip(over)) ? over : -1;
    }
    if (in.focus >= static_cast<int>(layout.items.size()) ||
        (in.focus >= 0 && (!Interactive(layout.items[static_cast<std::size_t>(in.focus)]) ||
                           (question && layout.items[static_cast<std::size_t>(in.focus)].action != ItemAction::Answer)))) {
      in.focus = -1;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
  }

  // A question over the page (MainWindow::Ask): whatever was going on in it
  // stops, the card comes in.
  void Ask(UiQuestion asked, std::function<void()> yes, std::function<void()> other = {}) {
    EndEdit(true);
    StopPan();
    selecting = false;
    barGrab.reset();
    question = std::move(asked);
    onYes = std::move(yes);
    onOther = std::move(other);
    onDate = nullptr;
    in.focus = in.pressed = in.tip = -1;
    in.focusVisible = false;
    questionAt = std::chrono::steady_clock::now();
    painter.questionShown = Motion() ? 0.0f : 1.0f;
    Relayout();
  }

  // The question answered (1 yes, 2 the other way, 0 no) or dropped: the page takes clicks again.
  void Answer(int which) {
    if (!question) {
      return;
    }
    if (which >= kAnswerMonthBack && question->calendar) {
      // A calendar's key: the date moves, the card stays.
      UiCalendar& cal = *question->calendar;
      switch (which) {
        case kAnswerMonthBack: CalendarShiftMonths(cal, -1); break;
        case kAnswerMonthOn: CalendarShiftMonths(cal, 1); break;
        case kAnswerYearBack: CalendarShiftMonths(cal, -12); break;
        case kAnswerYearOn: CalendarShiftMonths(cal, 12); break;
        default:
          if (which > kAnswerDay) {
            cal.day = std::clamp(which - kAnswerDay, 1, DaysInMonth(cal.year, cal.month));
          }
          break;
      }
      Relayout();
      return;
    }
    const std::function<void()> yes = std::exchange(onYes, nullptr);
    const std::function<void()> other = std::exchange(onOther, nullptr);
    const std::function<void(const UiCalendar&)> picked = std::exchange(onDate, nullptr);
    const std::optional<UiCalendar> date = question->calendar;
    question.reset();
    in.focus = in.pressed = -1;
    Relayout();
    if (which == 1 && date && picked) {
      picked(*date);
      return;
    }
    const std::function<void()>& then = which == 1 ? yes : other;
    if (which != 0 && then) {
      then();
    }
  }

  // A date asked on a calendar (MainWindow::AskDate): yes hands over what it
  // holds, the arrows, Page Up/Down (with Ctrl: the year) and Enter work it.
  void AskDate(UiQuestion asked, std::function<void(const UiCalendar&)> picked, std::function<void()> other) {
    Ask(std::move(asked), nullptr, std::move(other));
    onDate = std::move(picked);
  }

  // A key on a calendar's card: false if the question has none or the key
  // isn't one of its.
  bool CalendarKey(WPARAM key, bool ctrl) {
    if (!question || !question->calendar) {
      return false;
    }
    UiCalendar& cal = *question->calendar;
    switch (key) {
      case VK_LEFT: CalendarShiftDays(cal, -1); break;
      case VK_RIGHT: CalendarShiftDays(cal, 1); break;
      case VK_UP: CalendarShiftDays(cal, -7); break;
      case VK_DOWN: CalendarShiftDays(cal, 7); break;
      case VK_PRIOR: CalendarShiftMonths(cal, ctrl ? -12 : -1); break;
      case VK_NEXT: CalendarShiftMonths(cal, ctrl ? 12 : 1); break;
      case VK_HOME: cal.day = 1; break;
      case VK_END: cal.day = DaysInMonth(cal.year, cal.month); break;
      default: return false;
    }
    Relayout();
    return true;
  }

  // An edit box over the item for `anchor` and `index` on the page shown
  // (RenameProfile: over the title).
  void EditOver(UiCommand anchor, int index, UiCommand onEnter, const std::wstring& initial, bool digitsOnly) {
    for (const Item& it : layout.items) {
      const bool title = anchor == UiCommand::RenameProfile && it.kind == Kind::Title;
      const bool button = it.action == ItemAction::Command && it.command == anchor && it.index == index;
      if (title || button) {
        D2D1_RECT_F rect = it.rect;
        if (anchor == UiCommand::BoardAdd || anchor == UiCommand::ChipMenu) {
          // A "+" in a bucket's corner: a site is typed across the bucket, up
          // to the "+"; a chip: what's in it, or its name, across the bucket.
          for (const Item& card : layout.items) {
            if (card.kind == Kind::Card && card.rect.top <= it.rect.top && card.rect.bottom >= it.rect.bottom &&
                card.rect.left <= it.rect.left && card.rect.right >= it.rect.right) {
              rect = {card.rect.left + 8, it.rect.top,
                      anchor == UiCommand::ChipMenu ? card.rect.right - 8 : it.rect.left - 4, it.rect.bottom};
            }
          }
        }
        if (anchor == UiCommand::CheckRoute) {
          // Its button is an icon: the address is typed over the line's result.
          for (const Item& row : layout.items) {
            if (row.kind == Kind::CheckRow && row.rect.top < it.rect.bottom && row.rect.bottom > it.rect.top) {
              rect = {row.rect.left + 30 + TitleWidth(row) + kHint + 16, it.rect.top, it.rect.left - 4,
                      it.rect.bottom};
            }
          }
        }
        BeginEdit(rect, onEnter, index, initial, digitsOnly);
        return;
      }
    }
  }

  float TitleWidth(const Item& row) const { return painter.BodyWidth(row.text); }

  // Right-click on an item: what can be done with it - a server, a
  // configuration, a check, a rule, a program, the exit, a field. The row
  // under a "?" or a button counts; nothing for the rest.
  void ItemMenu(POINT screen) {
    POINT client = screen;
    ScreenToClient(hwnd, &client);
    const float x = ToDip(client.x);
    const float y = ToDip(client.y);
    const Item* found = nullptr;
    for (std::size_t i = layout.items.size(); i-- > 0;) {
      const Item& it = layout.items[i];
      const bool rowKind = it.kind == Kind::Choice || it.kind == Kind::ProfileRow || it.kind == Kind::CheckRow ||
                           it.kind == Kind::Switch || it.kind == Kind::AppRow ||
                           it.kind == Kind::Field || it.kind == Kind::ServerCard;
      if (rowKind && Contains(it.rect, x, y) && (!it.scrolls || (y >= layout.viewTop && y < layout.viewBottom))) {
        found = &it;
        break;
      }
    }
    if (found == nullptr) {
      return;
    }
    const Item item = *found;  // a copy: a command may relayout
    UiArgs args;
    args.index = item.index;
    args.sub = item.sub;
    args.owner = hwnd;
    args.anchor = screen;
    if (item.kind == Kind::ProfileRow && item.action == ItemAction::Command && item.command == UiCommand::RuleMenu) {
      onCommand(UiCommand::RuleMenu, args);  // a rule's menu is its click's
      return;
    }

    wil::unique_hmenu menu(CreatePopupMenu());
    if (!menu) {
      return;
    }
    const auto add = [&](UINT id, const std::wstring& text, bool enabled = true) {
      AppendMenuW(menu.get(), enabled ? MF_STRING : MF_STRING | MF_GRAYED, id, text.c_str());
    };
    const auto separator = [&] { AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr); };
    const auto at = [](const auto& list, int i) { return i >= 0 && static_cast<std::size_t>(i) < list.size(); };
    std::wstring name = item.text;  // what "copy the name" copies
    std::wstring ip;
    const UiProfile* profile = nullptr;
    switch (item.kind) {
      case Kind::Choice: {
        // Only a Hysteria 2 server has a menu: its speed - every Hysteria 2
        // server's (RoutingSettings::hysteriaMbps), the one set ticked. The
        // rest went (17.10.2026, he saw no use): picking is the row's click,
        // the latency the stopwatch's, copying its name or exit nobody's.
        const auto i = static_cast<std::size_t>(item.index);
        if (item.command != UiCommand::SetProtocol || i >= content.protocolTitles.size() ||
            !content.protocolTitles[i].starts_with(L"Hysteria 2")) {
          return;
        }
        AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, L"Скорость Hysteria 2 (на все её серверы)");
        separator();
        for (std::size_t s = 0; s < kHysteriaSpeeds.size(); ++s) {
          const int speed = kHysteriaSpeeds.at(s);
          const std::wstring text = speed == 0 ? std::wstring(L"Авто (BBR) — сервер подбирает сам")
                                               : std::format(L"{} Мбит/с (Brutal)", speed);
          AppendMenuW(menu.get(), MF_STRING | (speed == content.routing.hysteriaMbps ? MF_CHECKED : 0U),
                      kMenuHysteriaSpeed + static_cast<UINT>(s), text.c_str());
        }
        separator();
        AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, L"Brutal шлёт ровно с этой скоростью и не сбавляет");
        AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, L"при потерях: не ставь больше скорости своего канала.");
        break;
      }
      case Kind::ProfileRow:
        if (item.action != ItemAction::OpenProfile || !at(content.profiles, item.index)) {
          return;
        }
        profile = &content.profiles[static_cast<std::size_t>(item.index)];
        add(kMenuOpen, L"Открыть");
        add(kMenuToggle, profile->enabled ? L"Выключить" : L"Включить");
        if (profile->subscription) {
          add(kMenuRefresh, L"Обновить");
          if (profile->heldBack) {
            add(kMenuRefreshDirect, L"Скачать напрямую (сервер увидит IP)…");
          }
          add(kMenuCopyLink, L"Копировать ссылку");
        }
        add(kMenuRename, L"Переименовать");
        separator();
        add(kMenuRemove, L"Удалить…");
        break;
      case Kind::CheckRow: {
        const bool running = content.checksRunning;
        add(kMenuRun,
            item.sub == static_cast<int>(kCheckSpeed)   ? L"Замерить скорость"
            : item.sub == static_cast<int>(kCheckRoute) ? L"Проверить адрес…"
                                                        : L"Проверить снова",
            !running);
        separator();
        add(kMenuCopyResult, L"Копировать результат", item.index > 1);
        add(kMenuCopyAll, L"Копировать все результаты");
        break;
      }
      case Kind::AppRow:
        add(kMenuRemove, L"Убрать из списка");
        add(kMenuCopyName, L"Копировать имя");
        break;
      case Kind::ServerCard:
        add(kMenuOpen, L"Выбрать сервер…", item.enabled);
        add(kMenuTestDelays, L"Проверить задержку", true);
        separator();
        add(kMenuCopyIp, content.exitIp.empty() ? std::wstring(L"Копировать IP выхода")
                                                : L"Копировать IP выхода: " + content.exitIp,
            !content.exitIp.empty());
        add(kMenuToggleExitIp, content.hideExitIp ? L"Показывать IP" : L"Скрыть IP");
        ip = content.exitIp;
        break;
      case Kind::Field:
        add(kMenuCopyValue, L"Копировать «" + item.detail + L"»", !item.detail.empty());
        break;
      default: return;
    }
    const auto command = static_cast<UINT>(TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                          screen.x, screen.y, 0, hwnd, nullptr));
    if (command >= kMenuHysteriaSpeed && command < kMenuHysteriaSpeed + kHysteriaSpeeds.size()) {
      args.index = kHysteriaSpeeds.at(command - kMenuHysteriaSpeed);
      onCommand(UiCommand::SetHysteriaMbps, args);
      return;
    }
    switch (command) {
      case kMenuTestDelays: onCommand(UiCommand::TestDelays, args); break;
      case kMenuCopyName: CopyToClipboard(hwnd, name); break;
      case kMenuCopyIp: CopyToClipboard(hwnd, ip); break;
      case kMenuCopyValue: CopyToClipboard(hwnd, item.detail); break;
      case kMenuOpen:
        if (item.kind == Kind::ServerCard) {
          Go(UiPage::Servers);
        } else if (profile != nullptr) {
          openProfile = profile->id;
          Go(UiPage::Profile);
        }
        break;
      case kMenuToggle:
        onCommand(UiCommand::ToggleProfile, args);
        break;
      case kMenuRefresh: onCommand(UiCommand::RefreshProfile, args); break;
      case kMenuRefreshDirect: onCommand(UiCommand::RefreshDirect, args); break;
      case kMenuCopyLink: onCommand(UiCommand::CopyProfileLink, args); break;
      case kMenuRename:
        if (profile != nullptr) {
          const std::wstring current = profile->name;
          openProfile = profile->id;
          Go(UiPage::Profile);
          EditOver(UiCommand::RenameProfile, args.index, UiCommand::RenameProfile, current, false);
        }
        break;
      case kMenuRemove:
        onCommand(item.kind == Kind::AppRow ? UiCommand::RemoveApp : UiCommand::RemoveProfile, args);
        break;
      case kMenuRun:
        args.index = 0;
        onCommand(item.sub == static_cast<int>(kCheckSpeed)   ? UiCommand::RunSpeed
                  : item.sub == static_cast<int>(kCheckRoute) ? UiCommand::CheckRoute
                                                              : UiCommand::RunChecks,
                  args);
        break;
      case kMenuCopyResult:
        if (at(content.checks, item.sub)) {
          CopyToClipboard(hwnd, CheckText(content.checks[static_cast<std::size_t>(item.sub)]));
        }
        break;
      case kMenuCopyAll: {
        std::wstring all;
        for (const UiCheck& check : content.checks) {
          all += CheckText(check) + L"\r\n";
        }
        CopyToClipboard(hwnd, all);
        break;
      }
      case kMenuToggleExitIp: onCommand(UiCommand::ToggleExitIp, args); break;
      default: break;
    }
  }

  // A check as text: "Утечка DNS: i3D.net (DE) — резолверы не в России."
  static std::wstring CheckText(const UiCheck& check) {
    std::wstring text = check.title + L": " + (check.status == UiCheck::Status::NotRun ? L"не проверялось" : check.summary);
    if (!check.detail.empty()) {
      text += L" — " + check.detail;
    }
    return text;
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
    KickLive();
    KickCube();  // back on the overview, the cube's idle life starts again
  }

  void Activate(const Item& item) {
    switch (item.action) {
      case ItemAction::None:
      case ItemAction::Hint: return;
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
      case ItemAction::ToggleMore:
        settingsMore = !settingsMore;
        NoteFolds();
        Relayout();
        return;
      case ItemAction::Hide: PostMessageW(hwnd, WM_CLOSE, 0, 0); return;
      case ItemAction::Answer: Answer(item.index); return;
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
      UiQuestion note;
      note.title = L"Не удалось записать файл";
      note.text = L"Может быть, папка только для чтения или файл открыт в другой программе. Попробуй сохранить в другое место.";
      note.yes = L"Понятно";
      note.no.clear();
      note.tone = UiQuestion::Tone::Warning;
      Ask(std::move(note), nullptr);
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
      if (Interactive(layout.items[i]) && layout.items[i].kind != Kind::Hint && layout.items[i].kind != Kind::Day &&
          (!question || layout.items[i].action == ItemAction::Answer)) {
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
    // A name is short; a rule's sites, a DNS server's URL may be long (64 cut
    // them silently before 0.4.85).
    const bool name = onEnter == UiCommand::RenameProfile || onEnter == UiCommand::ChipRuleName;
    SendMessageW(edit.box.get(), EM_LIMITTEXT, digitsOnly || name ? 64 : 300, 0);
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
        if (tipDue && std::chrono::steady_clock::now() >= *tipDue) {
          tipDue.reset();
          KillTimer(w, kTipTimer);
          if (HasTip(in.hover)) {
            in.tip = in.hover;
          }
        }
        const bool going = liveAt && StepLiveTraffic();
        const bool easing = StepSpeed();
        const bool coming = StepQuestion();
        const bool folding = StepFold();
        PAINTSTRUCT ps;
        BeginPaint(w, &ps);
        Paint();
        EndPaint(w, &ps);
        if (going || easing || coming || folding) {
          InvalidateRect(w, nullptr, FALSE);  // the next frame: this one waited for the screen's refresh
        }
        return 0;
      }
      case WM_ENABLE:
        if (wParam != FALSE) {
          KickLive();  // the modal box is gone (Blocked)
        }
        return 0;
      case WM_ERASEBKGND: return 1;  // Direct2D paints everything
      case WM_NCCALCSIZE:
        if (wParam == TRUE) {
          // No system title bar: ours is in the client (Painter::Caption);
          // the sides and the bottom keep their sizing borders.
          auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);  // NOLINT(performance-no-int-to-ptr)
          const LONG top = params->rgrc[0].top;
          const LRESULT result = DefWindowProcW(w, message, wParam, lParam);
          params->rgrc[0].top = top;
          return result;
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_NCHITTEST: {
        // The sides, the bottom, the corners - the system's; the top edge
        // sizes it; anything not taking a click drags it - the title bar but
        // for its buttons, the cards, the empty space (he asked, 22.10.2026).
        // The log keeps its press: it selects lines, its bar scrolls.
        const LRESULT hit = DefWindowProcW(w, message, wParam, lParam);
        if (hit != HTCLIENT) {
          return hit;
        }
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(w, &at);
        const UINT dpi = GetDpiForWindow(w);
        const int frame = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        if (at.y < frame) {
          RECT rc{};
          GetClientRect(w, &rc);
          return at.x < frame * 2 ? HTTOPLEFT : at.x >= rc.right - frame * 2 ? HTTOPRIGHT : HTTOP;
        }
        if (HitAt(at) >= 0 ||
            (!question && (InLog(at) || log.pan.has_value() || barGrab.has_value() || selecting))) {
          return HTCLIENT;
        }
        return HTCAPTION;
      }
      case WM_SIZE:
        EndEdit(true);
        if (target) {
          target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        Relayout();
        if (wParam != SIZE_MINIMIZED) {
          KickCube();  // restored from the taskbar: the idle timer stopped while it was minimized
        }
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
        if (log.pan) {
          SetCursor(PanCursor());  // captured: no WM_SETCURSOR comes
          return 0;
        }
        if (barGrab) {
          DragLogThumb(ToDip(at.y) - *barGrab);
          return 0;
        }
        if (const bool hot = page == UiPage::Logs && !question && OnLogBar(at); hot != log.barHot) {
          log.barHot = hot;
          InvalidateRect(w, nullptr, FALSE);
        }
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
        Hover(HitAt(at));
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, w, 0};
        TrackMouseEvent(&tme);
        return 0;
      }
      case WM_MOUSELEAVE:
        mouse.reset();
        log.barHot = barGrab.has_value();
        Hover(-1);
        return 0;
      case WM_TIMER:
        if (wParam == kCubeTimer) {
          // One more frame after the last move: it lands on the pose exactly.
          // At rest the timer slows to the idle life's pace (and a turn
          // begun meanwhile is KickCube's, back at the quick one).
          if (IsWindowVisible(w) == FALSE || (!CubeMoving(GetTickCount64()) && !CubeIdling())) {
            KillTimer(w, kCubeTimer);
          } else if (!CubeMoving(GetTickCount64())) {
            SetTimer(w, kCubeTimer, kCubeIdleFrameMs, nullptr);
          }
          InvalidateRect(w, nullptr, FALSE);
          return 0;
        }
        if (wParam == kPanTimer) {
          if (log.pan) {
            PanStep();
          }
          return 0;
        }
        if (wParam == kTipTimer) {
          KillTimer(w, kTipTimer);
          tipDue.reset();
          if (HasTip(in.hover)) {
            in.tip = in.hover;
            InvalidateRect(w, nullptr, FALSE);
          }
          return 0;
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_LBUTTONDOWN: {
        const POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (log.pan) {
          StopPan();  // a press ends it, and does nothing else
          return 0;
        }
        SetFocus(w);
        SetCapture(w);
        in.focusVisible = false;
        // A press puts a tip away (a "?"'s stays: it's what it's for), and it
        // stays away until the pointer comes to something else.
        KillTimer(w, kTipTimer);
        tipDue.reset();
        if (!IsKind(in.tip, Kind::Hint)) {
          in.tip = -1;
        }
        if (question) {
          in.pressed = HitAt(at);  // its buttons only: the log under it keeps its lines
          InvalidateRect(w, nullptr, FALSE);
          return 0;
        }
        if (const auto thumb = page == UiPage::Logs && OnLogBar(at) && layout.logBox ? LogThumb(*layout.logBox, log)
                                                                                     : std::nullopt) {
          // The thumb taken where pressed; a press beside it brings it there.
          const float y = ToDip(at.y);
          const float grab = y >= thumb->top && y < thumb->bottom ? y - thumb->top : (thumb->bottom - thumb->top) / 2;
          barGrab = grab;
          log.barHot = true;
          DragLogThumb(y - grab);
          return 0;
        }
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
        if (barGrab) {
          barGrab.reset();
          ReleaseCapture();
          log.barHot = mouse && OnLogBar(*mouse);
          InvalidateRect(w, nullptr, FALSE);
          return 0;
        }
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
        barGrab.reset();
        StopPan();
        return 0;
      case WM_MBUTTONDOWN: {
        // The middle button on the log: scrolling by where the pointer goes.
        const POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (log.pan) {
          StopPan();
        } else if (!question && page == UiPage::Logs && layout.logBox && InLog(at) && LogThumb(*layout.logBox, log)) {
          StartPan(at);
        }
        return 0;
      }
      case WM_MBUTTONUP:
        if (log.pan && (panMoved || GetTickCount64() - panSince >= kPanHoldMs)) {
          StopPan();
        }
        return 0;
      case WM_RBUTTONDOWN:
        if (log.pan) {
          StopPan();
          panEndedByRight = true;
          return 0;
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_MOUSEWHEEL: {
        StopPan();
        if (question) {
          return 0;  // the page stays where the question found it
        }
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
        if (std::exchange(panEndedByRight, false) || question) {
          return 0;
        }
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
        if (POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; at.x != -1 || at.y != -1) {
          ItemMenu(at);
          return 0;
        }
        return DefWindowProcW(w, message, wParam, lParam);
      case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
          StopPan();
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
      case WM_SHOWWINDOW:
        if (wParam == FALSE) {
          Answer(0);  // hidden, a question goes unanswered: no
          // Hidden, the window hears no more of the pointer: where it was is
          // no longer where it is, and the window shown lights nothing up.
          mouse.reset();
          Hover(-1);
        }
        return DefWindowProcW(w, message, wParam, lParam);
      default:
        return DefWindowProcW(w, message, wParam, lParam);
    }
  }

  bool Key(WPARAM key) {
    if (log.pan) {
      StopPan();  // a key ends the middle button's scrolling, and does nothing else
      return true;
    }
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (question) {
      // Only its buttons: the page under it waits for the answer. Enter
      // with neither in focus is yes.
      if (CalendarKey(key, ctrl)) {
        return true;
      }
      switch (key) {
        case VK_ESCAPE: Answer(0); return true;
        case VK_TAB:
        case VK_LEFT:
        case VK_RIGHT: MoveFocus(shift || key == VK_LEFT); return true;
        case VK_RETURN:
        case VK_SPACE:
          if (in.focus >= 0 && static_cast<std::size_t>(in.focus) < layout.items.size() &&
              layout.items[static_cast<std::size_t>(in.focus)].action == ItemAction::Answer) {
            const Item item = layout.items[static_cast<std::size_t>(in.focus)];
            Activate(item);
          } else if (key == VK_RETURN) {
            Answer(1);
          }
          return true;
        default: return true;
      }
    }
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
      // Back (BackOf); from the overview, away.
      if (page != UiPage::Overview) {
        Go(BackOf(page));
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
    in.cube = cubeKnown ? std::optional(CubeAt(GetTickCount64())) : std::nullopt;
    target->BeginDraw();
    painter.Draw(target.get(), layout, content, in, log, &live);
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
  m.KickCube();  // turning while it connects: the timer stopped while hidden
  m.KickLive();
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
  Impl& m = *impl_;
  m.CubeTo(content.display);
  const bool sameList = content.protocols == m.content.protocols && content.autoOption == m.content.autoOption &&
                        content.serversByDelay == m.content.serversByDelay;
  if (!content.delaysTesting || !sameList || m.serverOrder.empty()) {
    m.serverOrder = ServerOrder(content);
  }
  m.content = content;
  m.NoteSpeed();
  m.NoteFolds();
  if (IsVisible()) {
    m.Relayout();
    m.KickLive();
  }
}

void MainWindow::EditInPlace(UiCommand anchor, int index, UiCommand onEnter, const std::wstring& initial,
                             bool digitsOnly) {
  impl_->EditOver(anchor, index, onEnter, initial, digitsOnly);
}

void MainWindow::Ask(UiQuestion question, std::function<void()> onYes, std::function<void()> onOther) {
  if (!IsVisible()) {
    Show(impl_->page);
  }
  impl_->Ask(std::move(question), std::move(onYes), std::move(onOther));
}

void MainWindow::AskDate(UiQuestion question, std::function<void(const UiCalendar&)> onDate,
                         std::function<void()> onOther) {
  if (!IsVisible()) {
    Show(impl_->page);
  }
  impl_->AskDate(std::move(question), std::move(onDate), std::move(onOther));
}

void MainWindow::SetLogs(const std::vector<std::wstring>& lines) { impl_->AddLogs(lines, true); }

void MainWindow::AppendLogs(const std::vector<std::wstring>& lines) {
  if (!lines.empty()) {
    impl_->AddLogs(lines, false);
  }
}

std::wstring DescribeMainWindowLayout(const UiContent& content, UiPage page, float width, float height) {
  const Painter painter;
  float scroll = 0;
  const std::string profile = content.profiles.empty() ? std::string() : content.profiles.front().id;
  const Layout layout = painter.Build(content, page, profile, width, height, scroll, LogState{});
  return painter.Describe(layout, width);
}

void RenderMainWindowSnapshot(const UiContent& content, UiPage page, const std::vector<std::wstring>& logs, UINT width,
                              UINT height, float dpi, const std::wstring& pngPath, std::wstring_view tipFor) {
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
  // The live traffic as it would be a few seconds in, an ad broken on the cube among it.
  LiveTraffic live;
  if (layout.glow) {
    for (int i = 0; i < 100; ++i) {
      UiContent now = content;
      now.blockedCount = i < 60 ? 0 : content.blockedCount;
      StepLive(live, now, content.display == Display::On, 1.0f / 30, *layout.glow);
    }
  }
  Interaction in;
  if (!tipFor.empty()) {
    const auto it = std::find_if(layout.items.begin(), layout.items.end(),
                                 [&](const Item& item) { return TipText(item).starts_with(tipFor); });
    THROW_HR_IF(E_INVALIDARG, it == layout.items.end());
    in.tip = static_cast<int>(it - layout.items.begin());
    in.hover = in.tip;
  }
  target->BeginDraw();
  painter.Draw(target.get(), layout, content, in, log, &live);
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
