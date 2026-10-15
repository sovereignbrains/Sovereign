#include "cube.h"

#include <wil/com.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "ui_style.h"

namespace sovereign::tray {

namespace {

constexpr float kDegrees = std::numbers::pi_v<float> / 180;

D2D1_COLOR_F Color(float r, float g, float b, float a = 1) { return D2D1::ColorF(r, g, b, a); }

D2D1_COLOR_F FromRef(COLORREF c, float a = 1) {
  return D2D1::ColorF(static_cast<float>(GetRValue(c)) / 255, static_cast<float>(GetGValue(c)) / 255,
                      static_cast<float>(GetBValue(c)) / 255, a);
}

D2D1_COLOR_F Scale(D2D1_COLOR_F c, float by) {
  return D2D1::ColorF(std::clamp(c.r * by, 0.0f, 1.0f), std::clamp(c.g * by, 0.0f, 1.0f),
                      std::clamp(c.b * by, 0.0f, 1.0f), c.a);
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, a); }

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
  return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

struct Vec {
  float x = 0;
  float y = 0;
  float z = 0;
};

float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Object space -> view space: yaw round the vertical, then the top tipped
// toward the viewer (+z) by pitch.
Vec Rotate(Vec v, float yaw, float pitch) {
  const float cy = std::cos(yaw * kDegrees);
  const float sy = std::sin(yaw * kDegrees);
  const float cp = std::cos(pitch * kDegrees);
  const float sp = std::sin(pitch * kDegrees);
  const Vec turned{v.x * cy + v.z * sy, v.y, -v.x * sy + v.z * cy};
  return {turned.x, turned.y * cp - turned.z * sp, turned.y * sp + turned.z * cp};
}

// Up, left and in front - the light the window's other things are lit by.
Vec Light() {
  const Vec l{-0.45f, 0.75f, 0.5f};
  const float n = std::sqrt(Dot(l, l));
  return {l.x / n, l.y / n, l.z / n};
}

struct FaceDef {
  Vec normal;
  std::array<Vec, 4> corners;  // of a unit cube (half-edge 1), round the face
};

constexpr std::array<FaceDef, 6> kFaces = {{
    {{1, 0, 0}, {{{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}}}},
    {{-1, 0, 0}, {{{-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}}}},
    {{0, 1, 0}, {{{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}}}},
    {{0, -1, 0}, {{{-1, -1, 1}, {1, -1, 1}, {1, -1, -1}, {-1, -1, -1}}}},
    {{0, 0, 1}, {{{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}}},
    {{0, 0, -1}, {{{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}}}},
}};

// How far the cube reaches below its center on screen, resting (no lift).
float ReachBelow(const CubePose& pose, float half) {
  float lowest = 0;
  for (const float x : {-1.0f, 1.0f}) {
    for (const float y : {-1.0f, 1.0f}) {
      for (const float z : {-1.0f, 1.0f}) {
        lowest = std::max(lowest, -Rotate({x * half, y * half, z * half}, pose.yaw, pose.pitch).y);
      }
    }
  }
  return lowest;
}

wil::com_ptr<ID2D1SolidColorBrush> Solid(ID2D1RenderTarget* t, D2D1_COLOR_F c) {
  wil::com_ptr<ID2D1SolidColorBrush> b;
  t->CreateSolidColorBrush(c, b.put());
  return b;
}

wil::com_ptr<ID2D1LinearGradientBrush> Linear(ID2D1RenderTarget* t, D2D1_POINT_2F from, D2D1_POINT_2F to,
                                              D2D1_COLOR_F a, D2D1_COLOR_F b) {
  const std::array<D2D1_GRADIENT_STOP, 2> stops{{{0, a}, {1, b}}};
  wil::com_ptr<ID2D1GradientStopCollection> collection;
  wil::com_ptr<ID2D1LinearGradientBrush> brush;
  if (SUCCEEDED(t->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put()))) {
    t->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection.get(), brush.put());
  }
  return brush;
}

// A soft spot: `c` at the middle fading to nothing at the ellipse's edge.
void Spot(ID2D1RenderTarget* t, D2D1_POINT_2F center, float rx, float ry, D2D1_COLOR_F c) {
  if (c.a <= 0.002f || rx <= 0 || ry <= 0) {
    return;
  }
  const std::array<D2D1_GRADIENT_STOP, 2> stops{{{0, c}, {1, WithAlpha(c, 0)}}};
  wil::com_ptr<ID2D1GradientStopCollection> collection;
  wil::com_ptr<ID2D1RadialGradientBrush> brush;
  if (FAILED(t->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put())) ||
      FAILED(t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center, {0, 0}, rx, ry),
                                          collection.get(), brush.put()))) {
    return;
  }
  t->FillEllipse(D2D1::Ellipse(center, rx, ry), brush.get());
}

wil::com_ptr<ID2D1PathGeometry> Polygon(ID2D1RenderTarget* t, const std::array<D2D1_POINT_2F, 4>& corners) {
  wil::com_ptr<ID2D1Factory> factory;
  t->GetFactory(factory.put());
  wil::com_ptr<ID2D1PathGeometry> path;
  wil::com_ptr<ID2D1GeometrySink> sink;
  if (!factory || FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) {
    return nullptr;
  }
  sink->BeginFigure(corners[0], D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLines(corners.data() + 1, static_cast<UINT32>(corners.size() - 1));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  return SUCCEEDED(sink->Close()) ? path : nullptr;
}


}  // namespace

CubePose CubePoseFor(Display display) {
  CubePose p;
  // Dark see-through glass, light thin edges - the same in every state; the
  // one colour is the core's, the state's. On, it shines through the glass
  // and round it; off, a grey ember.
  p.body = Color(0.12f, 0.13f, 0.15f, 0.62f);
  p.rim = Color(0.92f, 0.93f, 0.96f, 0.80f);
  p.signal = Color(0.62f, 0.64f, 0.70f, 0.55f);
  switch (display) {
    case Display::On:
      p.pitch = 32;
      p.lift = 6;
      p.glow = 1;
      p.press = 1;
      p.rim = Color(0.96f, 0.97f, 1.0f, 1.0f);
      p.signal = Mix(FromRef(ui::kAccent), Color(1, 1, 1), 0.15f);
      break;
    case Display::Starting:
      p.pitch = 29;
      p.lift = 3;
      p.glow = 0.5f;
      p.press = 0.55f;
      p.rim = Color(0.94f, 0.95f, 0.98f, 0.90f);
      p.signal = FromRef(ui::kWarning);
      break;
    case Display::Error:
      p.yaw = 20;  // askew: 25 degrees short of resting square
      p.pitch = 18;
      p.rim = Color(0.92f, 0.93f, 0.96f, 0.55f);
      p.signal = FromRef(ui::kDanger);
      break;
    case Display::ServiceDown:
      p.body = Color(0.10f, 0.10f, 0.11f, 0.62f);
      p.rim = Color(0.85f, 0.87f, 0.92f, 0.35f);
      p.signal = Color(0.6f, 0.62f, 0.66f, 0.5f);
      break;
    case Display::Off: break;
  }
  return p;
}

CubePose BlendPose(const CubePose& a, const CubePose& b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const auto lerp = [t](float x, float y) { return x + (y - x) * t; };
  return {.yaw = lerp(a.yaw, b.yaw),
          .pitch = lerp(a.pitch, b.pitch),
          .lift = lerp(a.lift, b.lift),
          .glow = lerp(a.glow, b.glow),
          .press = lerp(a.press, b.press),
          .body = Mix(a.body, b.body, t),
          .rim = Mix(a.rim, b.rim, t),
          .signal = Mix(a.signal, b.signal, t)};
}

float EaseOut(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const float rest = 1 - t;
  return 1 - rest * rest * rest;
}

float NextRestYaw(float yaw) {
  float on = std::fmod(45 - yaw, 90.0f);
  if (on < 0) {
    on += 90;
  }
  if (on < 30) {
    on += 90;
  }
  return yaw + on;
}

std::vector<CubeFace> CubeFaces(const CubePose& pose, D2D1_POINT_2F center, float half) {
  std::vector<CubeFace> faces;
  const Vec light = Light();
  for (std::size_t i = 0; i < kFaces.size(); ++i) {
    const FaceDef& def = kFaces[i];
    const Vec n = Rotate(def.normal, pose.yaw, pose.pitch);
    if (n.z <= 0.001f) {
      continue;  // edge-on or turned away
    }
    CubeFace face;
    face.top = i == 2;
    face.shade = std::clamp(0.25f + 0.75f * std::max(0.0f, Dot(n, light)), 0.0f, 1.0f);
    for (std::size_t c = 0; c < 4; ++c) {
      const Vec& v = def.corners[c];
      const Vec p = Rotate({v.x * half, v.y * half, v.z * half}, pose.yaw, pose.pitch);
      face.corners[c] = {center.x + p.x, center.y - p.y - pose.lift};
    }
    faces.push_back(face);
  }
  return faces;
}

void DrawCube(ID2D1RenderTarget* t, const CubePose& pose, D2D1_RECT_F box, bool hovered, bool pressed) {
  const CubePose& p = pose;
  const float size = box.right - box.left;
  const float half = size * 0.25f;
  const D2D1_POINT_2F center{(box.left + box.right) / 2, box.top + size * 0.48f};
  const D2D1_POINT_2F middle{center.x, center.y - p.lift};
  // How lit the core is: the state's, a little more under the pointer, less pressed.
  const float lit = std::clamp(p.press + (hovered ? 0.15f : 0.0f) - (pressed ? 0.25f : 0.0f), 0.0f, 1.0f);

  // The floor: a shadow, smaller and fainter the higher it floats, and the
  // core's light pooled on it.
  const float floor = center.y + ReachBelow(p, half) + 3;
  const float high = std::clamp(p.lift / 12, 0.0f, 1.0f);
  Spot(t, {center.x, floor}, half * 1.55f * (1 - 0.2f * high), half * 0.30f * (1 - 0.2f * high),
       Color(0, 0, 0, 0.55f * (1 - 0.35f * high)));
  Spot(t, {center.x, floor}, half * 1.3f, half * 0.24f, WithAlpha(p.signal, 0.30f * p.glow));
  // The light round it.
  Spot(t, middle, half * 2.6f, half * 2.6f, WithAlpha(p.signal, 0.20f * p.glow));

  // Its twelve edges: each between two faces; near if either faces the viewer.
  const auto at = [&](Vec v) {
    const Vec r = Rotate({v.x * half, v.y * half, v.z * half}, p.yaw, p.pitch);
    return D2D1_POINT_2F{center.x + r.x, center.y - r.y - p.lift};
  };
  const auto facing = [&](Vec n) { return Rotate(n, p.yaw, p.pitch).z > 0.001f; };
  struct Edge {
    D2D1_POINT_2F a;
    D2D1_POINT_2F b;
    int toward;  // how many of its two faces face the viewer: 0 behind the glass, 1 the outline, 2 inside it
  };
  std::vector<Edge> edges;
  for (int axis = 0; axis < 3; ++axis) {
    for (const float s : {-1.0f, 1.0f}) {
      for (const float u : {-1.0f, 1.0f}) {
        // The other two coordinates at s and u; the edge runs along `axis`.
        const Vec from = axis == 0 ? Vec{-1, s, u} : axis == 1 ? Vec{s, -1, u} : Vec{s, u, -1};
        const Vec to = axis == 0 ? Vec{1, s, u} : axis == 1 ? Vec{s, 1, u} : Vec{s, u, 1};
        const Vec n1 = axis == 0 ? Vec{0, s, 0} : Vec{s, 0, 0};
        const Vec n2 = axis == 2 ? Vec{0, u, 0} : Vec{0, 0, u};
        edges.push_back({at(from), at(to), (facing(n1) ? 1 : 0) + (facing(n2) ? 1 : 0)});
      }
    }
  }
  const D2D1_COLOR_F rim = Mix(p.rim, WithAlpha(p.signal, p.rim.a), 0.35f * p.glow);
  const auto ink = Solid(t, rim);

  // Behind the glass: the far edges, faint.
  if (ink) {
    ink->SetOpacity(0.16f);
    for (const Edge& e : edges) {
      if (e.toward == 0) {
        t->DrawLine(e.a, e.b, ink.get(), 1);
      }
    }
  }

  // The core: an orb of the state's colour, white at its heart while on.
  const float r = half * (0.26f + 0.32f * lit);
  Spot(t, middle, r * 2.6f, r * 2.6f, WithAlpha(p.signal, 0.55f * (0.35f + 0.65f * lit)));
  {
    const std::array<D2D1_GRADIENT_STOP, 3> stops{{{0, Mix(p.signal, Color(1, 1, 1), 0.75f * lit)},
                                                    {0.45f, WithAlpha(p.signal, 0.85f)},
                                                    {1, WithAlpha(p.signal, 0)}}};
    wil::com_ptr<ID2D1GradientStopCollection> collection;
    wil::com_ptr<ID2D1RadialGradientBrush> orb;
    if (SUCCEEDED(t->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put())) &&
        SUCCEEDED(t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(middle, {0, 0}, r, r),
                                               collection.get(), orb.put()))) {
      t->FillEllipse(D2D1::Ellipse(middle, r, r), orb.get());
    }
  }

  // The near faces: dark glass the core shows through - lit from above, the
  // core's colour on them, a gloss down from each one's top corner.
  const std::vector<CubeFace> faces = CubeFaces(p, center, half);
  for (const CubeFace& face : faces) {
    const auto path = Polygon(t, face.corners);
    if (!path) {
      continue;
    }
    const auto [topIt, bottomIt] = std::minmax_element(
        face.corners.begin(), face.corners.end(), [](const auto& a, const auto& b) { return a.y < b.y; });
    D2D1_COLOR_F glass = Scale(p.body, 0.55f + 0.75f * face.shade);
    glass = Mix(glass, WithAlpha(p.signal, glass.a), 0.16f * p.glow);
    if (const auto fill = Linear(t, *topIt, *bottomIt, Scale(glass, 1.15f), Scale(glass, 0.85f))) {
      t->FillGeometry(path.get(), fill.get());
    }
    const D2D1_POINT_2F mid{(topIt->x + bottomIt->x) / 2, (topIt->y + bottomIt->y) / 2};
    if (const auto gloss =
            Linear(t, *topIt, mid, Color(1, 1, 1, face.top ? 0.14f : 0.08f), Color(1, 1, 1, 0))) {
      t->FillGeometry(path.get(), gloss.get());
    }
  }
  // The core's light through the glass.
  Spot(t, middle, r * 1.8f, r * 1.8f, WithAlpha(Mix(p.signal, Color(1, 1, 1), 0.3f), 0.38f * lit));
  Spot(t, middle, r * 0.55f, r * 0.55f, Color(1, 1, 1, 0.55f * lit * lit));  // its white-hot heart

  // The near edges catch the light: the outline bright, the ones inside it softer.
  if (ink) {
    for (const Edge& e : edges) {
      if (e.toward > 0) {
        ink->SetOpacity(e.toward == 1 ? 0.95f : 0.55f);
        t->DrawLine(e.a, e.b, ink.get(), e.toward == 1 ? 1.3f : 1.0f);
      }
    }
  }
}

}  // namespace sovereign::tray
