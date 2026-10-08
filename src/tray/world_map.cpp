#include "world_map.h"

#include <wil/com.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "world_map.gen.h"

namespace sovereign::tray {

namespace {

constexpr float kDegrees = std::numbers::pi_v<float> / 180;

// Longitude from the focus, the short way round: -180..180.
float Wrap(float lon) {
  float d = std::fmod(lon + 540.0f, 360.0f) - 180.0f;
  return d < -180 ? d + 360 : d;
}

struct Vec3 {
  double x = 0;
  double y = 0;
  double z = 0;
};

Vec3 Unit(LatLon p) {
  const double lat = p.lat * kDegrees;
  const double lon = p.lon * kDegrees;
  return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
}

LatLon FromUnit(Vec3 v) {
  const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return {static_cast<float>(std::asin(std::clamp(v.z / n, -1.0, 1.0)) / kDegrees),
          static_cast<float>(std::atan2(v.y, v.x) / kDegrees)};
}

wil::com_ptr<ID2D1SolidColorBrush> Solid(ID2D1RenderTarget* t, D2D1_COLOR_F c) {
  wil::com_ptr<ID2D1SolidColorBrush> b;
  t->CreateSolidColorBrush(c, b.put());
  return b;
}

void Spot(ID2D1RenderTarget* t, D2D1_POINT_2F center, float rx, float ry, D2D1_COLOR_F c) {
  const std::array<D2D1_GRADIENT_STOP, 2> stops{{{0, c}, {1, D2D1::ColorF(c.r, c.g, c.b, 0)}}};
  wil::com_ptr<ID2D1GradientStopCollection> collection;
  wil::com_ptr<ID2D1RadialGradientBrush> brush;
  if (SUCCEEDED(t->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put())) &&
      SUCCEEDED(t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center, {0, 0}, rx, ry),
                                             collection.get(), brush.put()))) {
    t->FillEllipse(D2D1::Ellipse(center, rx, ry), brush.get());
  }
}

// How much of the map shows at a point: all of it round the middle, nothing
// at the window's edges - it lies under the cards, it mustn't fight them.
float Fade(D2D1_SIZE_F size, D2D1_POINT_2F center, D2D1_POINT_2F p) {
  const float dx = (p.x - center.x) / (size.width * 0.62f);
  const float dy = (p.y - center.y) / (size.height * 0.55f);
  return std::clamp(1.3f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
}

bool Inside(D2D1_SIZE_F size, D2D1_POINT_2F p) {
  return p.x > -8 && p.x < size.width + 8 && p.y > -8 && p.y < size.height + 8;
}

}  // namespace

std::optional<LatLon> CapitalOf(std::string_view country) {
  const auto it = std::find_if(world::kCapitals.begin(), world::kCapitals.end(),
                               [&](const world::Capital& c) { return c.country == country; });
  if (it == world::kCapitals.end()) {
    return std::nullopt;
  }
  return LatLon{it->lat, it->lon};
}

MapView FitMap(LatLon from, std::optional<LatLon> to, D2D1_POINT_2F center, float spread) {
  MapView view;
  view.center = center;
  view.focus = to ? GreatCircle(from, *to, 2)[1] : from;
  if (to) {
    // How far apart the ends lie on the map, in degrees at the focus.
    const float shrink = std::cos(view.focus.lat * kDegrees);
    const float dx = (Wrap(to->lon - view.focus.lon) - Wrap(from.lon - view.focus.lon)) * shrink;
    const float dy = (to->lat - from.lat) * view.squash;
    view.dipPerDegree = std::clamp(spread / std::max(std::hypot(dx, dy), 1.5f), 1.2f, 40.0f);
  }
  return view;
}

D2D1_POINT_2F Project(const MapView& view, LatLon at) {
  const float shrink = std::cos(view.focus.lat * kDegrees);
  return {view.center.x + Wrap(at.lon - view.focus.lon) * shrink * view.dipPerDegree,
          view.center.y - (at.lat - view.focus.lat) * view.squash * view.dipPerDegree};
}

std::vector<LatLon> GreatCircle(LatLon a, LatLon b, int steps) {
  steps = std::max(steps, 1);
  const Vec3 u = Unit(a);
  const Vec3 v = Unit(b);
  const double dot = std::clamp(u.x * v.x + u.y * v.y + u.z * v.z, -1.0, 1.0);
  const double angle = std::acos(dot);
  std::vector<LatLon> points;
  points.reserve(static_cast<std::size_t>(steps) + 1);
  for (int i = 0; i <= steps; ++i) {
    const double t = static_cast<double>(i) / steps;
    if (angle < 1e-6) {
      points.push_back(a);
      continue;
    }
    const double s = std::sin(angle);
    const double wa = std::sin((1 - t) * angle) / s;
    const double wb = std::sin(t * angle) / s;
    points.push_back(FromUnit({wa * u.x + wb * v.x, wa * u.y + wb * v.y, wa * u.z + wb * v.z}));
  }
  return points;
}

void DrawWorld(ID2D1RenderTarget* t, D2D1_SIZE_F size, const MapView& view, LatLon from, std::optional<LatLon> to,
               D2D1_COLOR_F signal, float arcHeight) {
  const auto ink = Solid(t, D2D1::ColorF(0.80f, 0.86f, 0.97f, 1));
  if (!ink) {
    return;
  }
  const D2D1_POINT_2F middle = view.center;
  // A cool glow under the map's middle.
  Spot(t, middle, size.width * 0.7f, size.height * 0.42f, D2D1::ColorF(0.09f, 0.14f, 0.30f, 0.30f));

  // A line of the map, faded with its place; broken where it crosses the
  // far side of the world (a jump across the window).
  const auto line = [&](LatLon a, LatLon b, float alpha) {
    const D2D1_POINT_2F p = Project(view, a);
    const D2D1_POINT_2F q = Project(view, b);
    if (std::abs(p.x - q.x) > size.width * 0.5f || (!Inside(size, p) && !Inside(size, q))) {
      return;
    }
    const float fade = Fade(size, middle, p);
    if (fade <= 0) {
      return;
    }
    ink->SetOpacity(alpha * fade);
    t->DrawLine(p, q, ink.get(), 1);
  };

  // The meridians and parallels every 10 degrees.
  for (float lat = -60; lat <= 80; lat += 10) {
    for (float lon = -180; lon < 180; lon += 4) {
      line({lat, lon}, {lat, lon + 4}, 0.06f);
    }
  }
  for (float lon = -180; lon < 180; lon += 10) {
    for (float lat = -60; lat < 85; lat += 2.5f) {
      line({lat, lon}, {lat + 2.5f, lon}, 0.06f);
    }
  }

  // The land as dots - every 2nd or 4th degree when they'd run together -
  // and its coastlines over them.
  const float spacing = view.dipPerDegree * std::cos(view.focus.lat * kDegrees);
  const int step = spacing < 2.5f ? 4 : spacing < 5 ? 2 : 1;
  const float r = std::clamp(spacing * 0.16f, 0.6f, 1.5f);
  for (const world::LandDot& dot : world::kLand) {
    if (dot.lat % step != 0 || dot.lon % step != 0) {
      continue;
    }
    const D2D1_POINT_2F p = Project(view, {static_cast<float>(dot.lat), static_cast<float>(dot.lon)});
    if (!Inside(size, p)) {
      continue;
    }
    const float fade = Fade(size, middle, p);
    if (fade <= 0) {
      continue;
    }
    ink->SetOpacity(0.30f * fade);
    t->FillEllipse(D2D1::Ellipse(p, r, r), ink.get());
  }
  for (std::size_t ring = 0; ring < world::kCoastStarts.size(); ++ring) {
    const std::size_t begin = world::kCoastStarts[ring];
    const std::size_t end = ring + 1 < world::kCoastStarts.size() ? world::kCoastStarts[ring + 1] : world::kCoast.size();
    for (std::size_t i = begin + 1; i < end; ++i) {
      line({static_cast<float>(world::kCoast[i - 1].lat) / 10, static_cast<float>(world::kCoast[i - 1].lon) / 10},
           {static_cast<float>(world::kCoast[i].lat) / 10, static_cast<float>(world::kCoast[i].lon) / 10}, 0.75f);
    }
  }

  // The user's country: a point and a ring.
  const D2D1_POINT_2F home = Project(view, from);
  ink->SetOpacity(0.95f);
  t->FillEllipse(D2D1::Ellipse(home, 2.6f, 2.6f), ink.get());
  ink->SetOpacity(0.4f);
  t->DrawEllipse(D2D1::Ellipse(home, 7, 7 * view.squash), ink.get(), 1);
  if (!to) {
    return;
  }
  // The route: up off the map and down again, over the cube - white at the
  // user's end, the state's colour at the exit's.
  const auto path = GreatCircle(from, *to, 64);
  std::vector<D2D1_POINT_2F> arc;
  arc.reserve(path.size());
  for (std::size_t i = 0; i < path.size(); ++i) {
    const D2D1_POINT_2F p = Project(view, path[i]);
    const float t01 = static_cast<float>(i) / static_cast<float>(path.size() - 1);
    arc.push_back({p.x, p.y - std::sin(std::numbers::pi_v<float> * t01) * arcHeight});
  }
  const std::array<D2D1_GRADIENT_STOP, 2> stops{{{0, D2D1::ColorF(0.9f, 0.93f, 1.0f, 0.9f)}, {1, signal}}};
  wil::com_ptr<ID2D1GradientStopCollection> collection;
  wil::com_ptr<ID2D1LinearGradientBrush> stroke;
  if (SUCCEEDED(t->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put())) &&
      SUCCEEDED(t->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(arc.front(), arc.back()),
                                             collection.get(), stroke.put()))) {
    stroke->SetOpacity(0.14f);
    for (std::size_t i = 1; i < arc.size(); ++i) {
      t->DrawLine(arc[i - 1], arc[i], stroke.get(), 6);
    }
    stroke->SetOpacity(0.95f);
    for (std::size_t i = 1; i < arc.size(); ++i) {
      t->DrawLine(arc[i - 1], arc[i], stroke.get(), 1.6f);
    }
  }
  const D2D1_POINT_2F exit = Project(view, *to);
  Spot(t, exit, 16, 16 * view.squash, D2D1::ColorF(signal.r, signal.g, signal.b, 0.35f));
  if (const auto end = Solid(t, signal)) {
    t->FillEllipse(D2D1::Ellipse(exit, 3, 3), end.get());
    end->SetOpacity(0.5f);
    t->DrawEllipse(D2D1::Ellipse(exit, 9, 9 * view.squash), end.get(), 1);
  }
}

}  // namespace sovereign::tray
