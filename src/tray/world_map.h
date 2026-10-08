#pragma once

#include <d2d1.h>

#include <optional>
#include <string_view>
#include <vector>

// The main screen's stage: the world as a map lying under the cube - the
// land as dots with its coastlines, the meridians and parallels as a
// drawing's lines - and the route the traffic takes, an arch from the
// user's country to the exit's over the cube standing between them.
// Countries only, by their capitals: nothing says where the user is closer
// than that.
//
// The map is seen the way the cube is: from above at a tilt, without
// perspective (cube.h) - every part at one scale, north drawn shorter by the
// tilt, nothing crowded against a horizon. The fit (FitMap) puts the
// route's middle at a chosen point and spreads its ends across the window.
// The math is pure, tested in tests/unit/world_map_test.cpp; the land and
// the capitals are Natural Earth's (world_map.gen.h, tools/worldmap).

namespace sovereign::tray {

struct LatLon {
  float lat = 0;
  float lon = 0;
};

// The capital of a country (ISO 3166-1 alpha-2, "RU"), if the map has it.
std::optional<LatLon> CapitalOf(std::string_view country);

// What the map shows where.
struct MapView {
  D2D1_POINT_2F center{};  // where `focus` is drawn
  LatLon focus;
  float dipPerDegree = 8;  // east-west, at the focus's latitude (a degree of longitude there)
  float squash = 0.6f;     // north-south drawn this much shorter: the tilt
};

// A view that puts the route's middle at `center` and its ends `spread`
// apart; with only `from`, that country at `center`.
MapView FitMap(LatLon from, std::optional<LatLon> to, D2D1_POINT_2F center, float spread);

// A point of the world on the screen.
D2D1_POINT_2F Project(const MapView& view, LatLon at);

// The great circle from `a` to `b` in `steps` + 1 points, both ends included.
std::vector<LatLon> GreatCircle(LatLon a, LatLon b, int steps);

// The stage on the whole of `size`: the map, fading toward the window's
// edges, and - with `to` - the route, an arch `arcHeight` high at its
// middle, its exit end in `signal`.
void DrawWorld(ID2D1RenderTarget* target, D2D1_SIZE_F size, const MapView& view, LatLon from,
               std::optional<LatLon> to, D2D1_COLOR_F signal, float arcHeight);

}  // namespace sovereign::tray
