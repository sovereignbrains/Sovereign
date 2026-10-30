#pragma once

// windows.h's min/max macros break std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <wil/com.h>

#include <string>

#include "ui_content.h"

namespace sovereign::tray::ui {

// The tray's look: dark only, Windows 11 proportions - an instrument: the
// graphite is silent, only a state is lit (30.10.2026, a review's "цвет не
// несёт одну семантику"). One colour, one meaning:
//  - your hand: a switch on, a pick, the main action - kLit, the light's
//    white, never a hue;
//  - where traffic goes: VPN blue (the cube's light), WARP orange, direct
//    grey, blocked red - the traffic's own dots, the routes' rails;
//  - how a check fares: green good, amber attention, red failed.
// A logo (a DNS provider's) is no colour of ours: not set among the values.

inline D2D1_COLOR_F Rgb(float r, float g, float b, float a = 1.0f) { return D2D1::ColorF(r / 255, g / 255, b / 255, a); }

inline D2D1_COLOR_F FromColorRef(COLORREF c, float a = 1.0f) {
  return Rgb(static_cast<float>(GetRValue(c)), static_cast<float>(GetGValue(c)), static_cast<float>(GetBValue(c)), a);
}

// Surfaces, darkest first: neutral near-black, a drawing's paper (the plan
// of 07.10 - no blue cast, no pure black). The card color is opaque on
// purpose: a child control (the log's edit box) paints with it through GDI.
inline constexpr COLORREF kWindowColor = RGB(14, 14, 15);
inline constexpr COLORREF kPanelColor = RGB(18, 18, 19);  // the window's caption, the navigation
inline constexpr COLORREF kCardColor = RGB(24, 24, 26);
inline constexpr COLORREF kPrimaryText = RGB(242, 244, 248);
inline constexpr COLORREF kSecondaryText = RGB(156, 158, 164);
// Your hand: what's on, what's picked; kLitInk is text on its fill.
inline constexpr COLORREF kLit = RGB(226, 229, 235);
inline constexpr COLORREF kLitInk = RGB(16, 17, 20);
// The traffic's ways.
inline constexpr COLORREF kVpn = RGB(76, 146, 255);
inline constexpr COLORREF kWarp = RGB(232, 134, 66);
inline constexpr COLORREF kDirect = RGB(160, 165, 176);
// A check's verdicts (and the blocked way's red).
inline constexpr COLORREF kUpload = RGB(64, 196, 140);
inline constexpr COLORREF kDanger = RGB(240, 96, 96);
inline constexpr COLORREF kWarning = RGB(235, 180, 60);

// A latency's color: fast green, slow amber, very slow or dead red.
inline COLORREF DelayColor(const UiDelay& delay) {
  if (delay.state == UiDelay::State::Failed) {
    return kDanger;
  }
  if (delay.state != UiDelay::State::Ok) {
    return kSecondaryText;
  }
  if (delay.ms < 200) {
    return kUpload;
  }
  return delay.ms < 500 ? kWarning : kDanger;
}

// A glyph font that exists here: Fluent Icons on Windows 11, MDL2 on 10 (the
// same code points).
inline std::wstring GlyphFamily(IDWriteFactory* factory) {
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

}  // namespace sovereign::tray::ui
