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

bool Near(D2D1_POINT_2F a, D2D1_POINT_2F b) { return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f; }

// A drafting line: short dashes, round nothing.
wil::com_ptr<ID2D1StrokeStyle> Dashed(ID2D1RenderTarget* t) {
  wil::com_ptr<ID2D1Factory> factory;
  t->GetFactory(factory.put());
  static constexpr std::array<float, 2> kDashes = {3, 4};
  wil::com_ptr<ID2D1StrokeStyle> style;
  if (factory) {
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                           D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10,
                                                           D2D1_DASH_STYLE_CUSTOM, 0),
                               kDashes.data(), static_cast<UINT32>(kDashes.size()), style.put());
  }
  return style;
}

// From `a` through `b` and `length` on.
D2D1_POINT_2F Beyond(D2D1_POINT_2F a, D2D1_POINT_2F b, float length) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float n = std::hypot(dx, dy);
  return n < 0.001f ? b : D2D1_POINT_2F{b.x + dx / n * length, b.y + dy / n * length};
}

// The knife switch on the cube's top face: a round mount (a circle seen at
// the tilt - an ellipse) with the groove the lever runs in, the hinge, the
// lever - turning in the plane that faces the viewer - and its knob, the
// one thing in the state's colour.
void DrawLever(ID2D1RenderTarget* t, const CubePose& p, const std::vector<CubeFace>& faces, float half) {
  const auto top = std::find_if(faces.begin(), faces.end(), [](const CubeFace& f) { return f.top; });
  const auto ink = Solid(t, p.rim);
  if (top == faces.end() || !ink) {
    return;
  }
  D2D1_POINT_2F base{0, 0};
  for (const auto& c : top->corners) {
    base.x += c.x / 4;
    base.y += c.y / 4;
  }
  const float sp = std::sin(p.pitch * kDegrees);
  const float cp = std::cos(p.pitch * kDegrees);

  // The mount, and the groove across it.
  const float r = half * 0.46f;
  if (const auto mount = Solid(t, Scale(p.body, 0.6f))) {
    t->FillEllipse(D2D1::Ellipse(base, r, r * sp), mount.get());
  }
  ink->SetOpacity(0.7f);
  t->DrawEllipse(D2D1::Ellipse(base, r, r * sp), ink.get(), 1);
  if (const auto groove = Solid(t, Color(0.02f, 0.02f, 0.025f, 1))) {
    t->DrawLine({base.x - r * 0.75f, base.y}, {base.x + r * 0.75f, base.y}, groove.get(), 3.2f);
  }

  // The lever, out of the hinge just above the face.
  const D2D1_POINT_2F hinge{base.x, base.y - half * 0.05f * cp};
  const float angle = LeverAngle(p.press) * kDegrees;
  const float length = half * 0.95f;
  const D2D1_POINT_2F tip{hinge.x + length * std::sin(angle), hinge.y - length * std::cos(angle) * cp};
  if (const auto dark = Solid(t, Color(0.02f, 0.02f, 0.025f, 0.9f))) {
    t->DrawLine(hinge, tip, dark.get(), 5.5f);
  }
  ink->SetOpacity(0.9f);
  t->DrawLine(hinge, tip, ink.get(), 2.6f);

  // The hinge pin.
  if (const auto pin = Solid(t, Scale(p.body, 1.6f))) {
    t->FillEllipse(D2D1::Ellipse(hinge, 3.4f, 3.4f), pin.get());
  }
  ink->SetOpacity(0.9f);
  t->DrawEllipse(D2D1::Ellipse(hinge, 3.4f, 3.4f), ink.get(), 1);

  // The knob: the state's colour, glowing a little round it.
  const float kr = half * 0.17f;
  Spot(t, tip, kr * 3.2f, kr * 3.2f, WithAlpha(p.signal, 0.30f * p.signal.a));
  if (const auto knob = Linear(t, {tip.x - kr, tip.y - kr}, {tip.x + kr, tip.y + kr}, Scale(p.signal, 1.15f),
                               Scale(p.signal, 0.7f))) {
    t->FillEllipse(D2D1::Ellipse(tip, kr, kr), knob.get());
  }
  ink->SetOpacity(0.85f);
  t->DrawEllipse(D2D1::Ellipse(tip, kr, kr), ink.get(), 1);
  if (const auto shine = Solid(t, Color(1, 1, 1, 0.45f))) {
    t->FillEllipse(D2D1::Ellipse({tip.x - kr * 0.35f, tip.y - kr * 0.38f}, kr * 0.28f, kr * 0.28f), shine.get());
  }
}

}  // namespace

CubePose CubePoseFor(Display display) {
  CubePose p;
  // A drawing's palette: dark glass, light thin edges - the same in every
  // state; the one colour is the lever's knob. On, the edges brighten and a
  // faint white light stands round it.
  p.body = Color(0.12f, 0.12f, 0.13f, 0.96f);
  p.rim = Color(0.92f, 0.93f, 0.96f, 0.80f);
  p.signal = Color(0.80f, 0.81f, 0.85f, 0.90f);
  switch (display) {
    case Display::On:
      p.pitch = 32;
      p.lift = 6;
      p.glow = 0.35f;
      p.press = 1;
      p.rim = Color(0.96f, 0.97f, 1.0f, 1.0f);
      p.signal = Mix(FromRef(ui::kAccent), Color(1, 1, 1), 0.15f);
      break;
    case Display::Starting:
      p.pitch = 29;
      p.lift = 3;
      p.glow = 0.15f;
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
      p.body = Color(0.10f, 0.10f, 0.11f, 0.95f);
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

float LeverAngle(float press) { return -38 + 76 * std::clamp(press, 0.0f, 1.0f); }

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
  CubePose p = pose;
  // The lever answers the pointer: under it, it leans toward the other side;
  // pushed, it goes most of the way.
  const float toward = p.press > 0.5f ? -1.0f : 1.0f;
  p.press = std::clamp(p.press + toward * (pressed ? 0.45f : hovered ? 0.15f : 0.0f), 0.0f, 1.0f);
  if (hovered && !pressed) {
    p.body = Scale(p.body, 1.08f);
  }
  const float size = box.right - box.left;
  const float half = size * 0.25f;
  const D2D1_POINT_2F center{(box.left + box.right) / 2, box.top + size * 0.48f};

  // The shadow on the floor: smaller and fainter the higher it floats.
  const float floor = center.y + ReachBelow(p, half) + 3;
  const float high = std::clamp(p.lift / 12, 0.0f, 1.0f);
  Spot(t, {center.x, floor}, half * 1.55f * (1 - 0.2f * high), half * 0.30f * (1 - 0.2f * high),
       Color(0, 0, 0, 0.55f * (1 - 0.35f * high)));
  // The light inside, spilling round it.
  Spot(t, {center.x, center.y - p.lift}, half * 2.4f, half * 2.4f, WithAlpha(p.rim, 0.26f * p.glow));

  const std::vector<CubeFace> faces = CubeFaces(p, center, half);
  for (const CubeFace& face : faces) {
    const auto path = Polygon(t, face.corners);
    if (!path) {
      continue;
    }
    // Lit from above: lighter at the face's top, darker at its foot; the
    // light inside warms the faces toward the rim's colour.
    const auto [topIt, bottomIt] = std::minmax_element(
        face.corners.begin(), face.corners.end(), [](const auto& a, const auto& b) { return a.y < b.y; });
    D2D1_COLOR_F lit = Scale(p.body, 0.45f + 0.85f * face.shade);
    lit = Mix(lit, WithAlpha(p.rim, lit.a), 0.18f * p.glow * face.shade);
    if (const auto fill = Linear(t, *topIt, *bottomIt, Scale(lit, 1.12f), Scale(lit, 0.86f))) {
      t->FillGeometry(path.get(), fill.get());
    }
    if (face.top) {  // matte sheen across the top, from its far corner
      if (const auto sheen = Linear(t, *topIt, *bottomIt, Color(1, 1, 1, 0.10f), Color(1, 1, 1, 0))) {
        t->FillGeometry(path.get(), sheen.get());
      }
    }
  }

  // The edges catch the light: where two lit faces meet, bright; the outline,
  // softer - and running on past the corners in dashes, a drawing's
  // construction lines; a point at each corner.
  const auto edges = Solid(t, p.rim);
  std::vector<std::pair<D2D1_POINT_2F, D2D1_POINT_2F>> outline;
  if (edges) {
    for (std::size_t i = 0; i < faces.size(); ++i) {
      for (std::size_t c = 0; c < 4; ++c) {
        const D2D1_POINT_2F a = faces[i].corners[c];
        const D2D1_POINT_2F b = faces[i].corners[(c + 1) % 4];
        bool shared = false;
        bool drawn = false;
        for (std::size_t j = 0; j < faces.size(); ++j) {
          for (std::size_t d = 0; j != i && d < 4; ++d) {
            const D2D1_POINT_2F x = faces[j].corners[d];
            const D2D1_POINT_2F y = faces[j].corners[(d + 1) % 4];
            if ((Near(a, x) && Near(b, y)) || (Near(a, y) && Near(b, x))) {
              shared = true;
              drawn = drawn || j < i;  // the other face drew it
            }
          }
        }
        if (drawn) {
          continue;
        }
        if (!shared) {
          outline.emplace_back(a, b);
        }
        edges->SetOpacity(shared ? 0.75f : 1.0f);
        t->DrawLine(a, b, edges.get(), shared ? 1.0f : 1.2f);
      }
    }
    const auto dashed = Dashed(t);
    edges->SetOpacity(0.22f);
    for (const auto& [a, b] : outline) {
      t->DrawLine(b, Beyond(a, b, half * 0.9f), edges.get(), 1, dashed.get());
      t->DrawLine(a, Beyond(b, a, half * 0.9f), edges.get(), 1, dashed.get());
    }
    edges->SetOpacity(0.95f);
    for (const auto& [a, b] : outline) {
      t->FillEllipse(D2D1::Ellipse(a, 1.7f, 1.7f), edges.get());
    }
  }

  DrawLever(t, p, faces, half);
}

void DrawBlueprint(ID2D1RenderTarget* t, D2D1_SIZE_F size, D2D1_POINT_2F focus) {
  const auto ink = Solid(t, Color(1, 1, 1, 1));
  if (!ink) {
    return;
  }
  constexpr float kCell = 40;
  // The grid, through the focus.
  ink->SetOpacity(0.028f);
  const float x0 = std::fmod(focus.x, kCell);
  const float y0 = std::fmod(focus.y, kCell);
  for (float x = x0; x < size.width; x += kCell) {
    t->DrawLine({x, 0}, {x, size.height}, ink.get(), 1);
  }
  for (float y = y0; y < size.height; y += kCell) {
    t->DrawLine({0, y}, {size.width, y}, ink.get(), 1);
  }
  // The isometric axes (30 degrees off the horizontal) and the vertical, in dashes.
  const auto dashed = Dashed(t);
  ink->SetOpacity(0.075f);
  const float reach = size.width + size.height;
  const float rise = std::tan(30 * kDegrees);
  for (const float side : {-1.0f, 1.0f}) {
    t->DrawLine({focus.x - reach * side, focus.y - reach * rise}, {focus.x + reach * side, focus.y + reach * rise},
                ink.get(), 1, dashed.get());
  }
  t->DrawLine({focus.x, 0}, {focus.x, size.height}, ink.get(), 1, dashed.get());
  // Crosses at every third crossing of the grid.
  ink->SetOpacity(0.16f);
  constexpr float kArm = 3.5f;
  for (float x = std::fmod(focus.x, kCell * 3); x < size.width; x += kCell * 3) {
    for (float y = std::fmod(focus.y, kCell * 3); y < size.height; y += kCell * 3) {
      t->DrawLine({x - kArm, y}, {x + kArm, y}, ink.get(), 1);
      t->DrawLine({x, y - kArm}, {x, y + kArm}, ink.get(), 1);
    }
  }
}

}  // namespace sovereign::tray
