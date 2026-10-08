// The main screen's stage (src/tray/world_map.h): the capitals, the great
// circle, and the map's fit and projection.

#include <cmath>
#include <exception>
#include <iostream>

#include "check.h"
#include "world_map.h"

namespace {

using namespace sovereign::tray;

bool Close(float a, float b, float within = 0.05f) { return std::abs(a - b) < within; }

void TestCapitals() {
  const auto moscow = CapitalOf("RU");
  const auto tallinn = CapitalOf("EE");
  CHECK(moscow && Close(moscow->lat, 55.75f, 0.2f) && Close(moscow->lon, 37.6f, 0.2f));
  CHECK(tallinn && Close(tallinn->lat, 59.4f, 0.2f));
  CHECK(!CapitalOf("XX") && !CapitalOf(""));
}

void TestGreatCircle() {
  const LatLon a{.lat = 55.75f, .lon = 37.6f};
  const LatLon b{.lat = 38.9f, .lon = -77.0f};
  const auto path = GreatCircle(a, b, 10);
  CHECK(path.size() == 11);
  CHECK(Close(path.front().lat, a.lat) && Close(path.back().lon, b.lon));
  // Moscow to Washington goes over the north, not along a parallel.
  float north = -90;
  for (const LatLon& p : path) {
    north = std::max(north, p.lat);
  }
  CHECK(north > 60);
  const auto same = GreatCircle(a, a, 4);
  CHECK(same.size() == 5 && Close(same[2].lat, a.lat));
}

void TestMap() {
  const LatLon moscow{.lat = 55.75f, .lon = 37.6f};
  const LatLon hague{.lat = 52.08f, .lon = 4.27f};
  const MapView view = FitMap(moscow, hague, {200, 400}, 260);
  // The route's middle at the center; its ends `spread` apart, Moscow east (right).
  const auto m = Project(view, GreatCircle(moscow, hague, 2)[1]);
  CHECK(Close(m.x, 200, 0.5f) && Close(m.y, 400, 0.5f));
  const auto east = Project(view, moscow);
  const auto west = Project(view, hague);
  CHECK(east.x > west.x && Close(std::hypot(east.x - west.x, east.y - west.y), 260, 1));
  // North is up, drawn shorter than east by the tilt; one scale everywhere.
  const auto n = Project(view, {.lat = 60, .lon = 20});
  const auto s = Project(view, {.lat = 50, .lon = 20});
  CHECK(n.y < s.y);
  const auto e = Project(view, {.lat = 50, .lon = 30});
  CHECK(Close((s.y - n.y) / 10, view.dipPerDegree * view.squash, 0.01f));
  CHECK(Close(e.x - s.x, Project(view, {.lat = 70, .lon = 30}).x - Project(view, {.lat = 70, .lon = 20}).x, 0.01f));
  // Longitude wraps the short way: 179 and -179 are neighbours.
  const MapView pacific = FitMap({.lat = 0, .lon = 179}, std::nullopt, {200, 400}, 260);
  CHECK(std::abs(Project(pacific, {.lat = 0, .lon = 179}).x - Project(pacific, {.lat = 0, .lon = -179}).x) < 50);
  // One country only: it at the center.
  const auto home = Project(FitMap(moscow, std::nullopt, {200, 400}, 260), moscow);
  CHECK(Close(home.x, 200, 0.5f) && Close(home.y, 400, 0.5f));
  // Far apart, zoomed out - never past the limits.
  const MapView wide = FitMap(moscow, LatLon{.lat = -33.9f, .lon = 151.2f}, {200, 400}, 260);
  CHECK(wide.dipPerDegree >= 1.2f && wide.dipPerDegree < view.dipPerDegree);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestCapitals();
    TestGreatCircle();
    TestMap();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
