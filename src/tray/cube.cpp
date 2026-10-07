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

}  // namespace

CubePose CubePoseFor(Display display) {
  CubePose p;
  switch (display) {
    case Display::On:
      p.pitch = 34;
      p.lift = 8;
      p.glow = 1;
      p.body = Scale(FromRef(ui::kAccent), 0.78f);
      p.rim = Color(0.80f, 0.88f, 1.0f, 0.95f);
      p.glyph = Color(0.97f, 0.98f, 1.0f, 1.0f);
      break;
    case Display::Starting:
      p.pitch = 30;
      p.lift = 4;
      p.glow = 0.35f;
      p.body = Color(0.30f, 0.33f, 0.40f, 0.96f);
      p.rim = Color(0.92f, 0.65f, 0.0f, 0.95f);
      p.glyph = Color(0.95f, 0.72f, 0.20f, 1.0f);
      break;
    case Display::Error:
      p.yaw = 20;  // askew: 25 degrees short of resting square
      p.pitch = 18;
      p.body = Color(0.36f, 0.22f, 0.24f, 0.96f);
      p.rim = FromRef(ui::kDanger, 0.9f);
      p.glyph = Color(1.0f, 0.62f, 0.62f, 0.95f);
      break;
    case Display::ServiceDown:
      p.body = Color(0.21f, 0.22f, 0.25f, 0.95f);
      p.rim = Color(0.70f, 0.72f, 0.78f, 0.25f);
      p.glyph = Color(0.80f, 0.82f, 0.88f, 0.40f);
      break;
    case Display::Off:
      p.body = Color(0.29f, 0.31f, 0.36f, 0.96f);
      p.rim = Color(0.78f, 0.81f, 0.88f, 0.40f);
      p.glyph = Color(0.86f, 0.88f, 0.93f, 0.80f);
      break;
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
          .body = Mix(a.body, b.body, t),
          .rim = Mix(a.rim, b.rim, t),
          .glyph = Mix(a.glyph, b.glyph, t)};
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

void DrawCube(ID2D1RenderTarget* t, const CubePose& pose, D2D1_RECT_F box, bool hovered, bool pressed,
              IDWriteTextFormat* glyphFormat, std::wstring_view glyph) {
  CubePose p = pose;
  if (pressed) {
    p.lift = std::max(0.0f, p.lift - 3);
    p.body = Scale(p.body, 0.92f);
  } else if (hovered) {
    p.lift += 2;
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

  // The edges catch the light: where two lit faces meet, bright; the outline, softer.
  const auto edges = Solid(t, p.rim);
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
        edges->SetOpacity(shared ? 1.0f : 0.55f);
        t->DrawLine(a, b, edges.get(), shared ? 1.4f : 1.0f);
      }
    }
  }

  // The power sign lying on the top face: as wide as the face, foreshortened
  // by the tilt, upright whatever the turn.
  if (glyphFormat != nullptr && !glyph.empty()) {
    const auto top = std::find_if(faces.begin(), faces.end(), [](const CubeFace& f) { return f.top; });
    if (top != faces.end()) {
      D2D1_POINT_2F mid{0, 0};
      for (const auto& c : top->corners) {
        mid.x += c.x / 4;
        mid.y += c.y / 4;
      }
      constexpr float kBox = 40;
      const float scale = half * 1.15f / kBox;
      const float squash = std::sin(p.pitch * kDegrees);
      D2D1_MATRIX_3X2_F before{};
      t->GetTransform(&before);
      const D2D1_MATRIX_3X2_F onTop = D2D1::Matrix3x2F(scale, 0, 0, scale * squash, mid.x - scale * kBox / 2,
                                                       mid.y - scale * squash * kBox / 2);
      t->SetTransform(onTop * before);
      if (const auto ink = Solid(t, WithAlpha(p.glyph, p.glyph.a * (0.55f + 0.45f * top->shade)))) {
        t->DrawText(glyph.data(), static_cast<UINT32>(glyph.size()), glyphFormat, D2D1::RectF(0, 0, kBox, kBox),
                    ink.get());
      }
      t->SetTransform(before);
    }
  }
}

}  // namespace sovereign::tray
