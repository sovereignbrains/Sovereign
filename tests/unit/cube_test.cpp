// The connect cube's pose (src/tray/cube.h): the turns between states, the
// faces it shows, the light on them.

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>

#include "check.h"
#include "cube.h"

namespace {

using namespace sovereign::tray;

bool Close(float a, float b) { return std::abs(a - b) < 0.01f; }

void TestRestYaw() {
  CHECK(Close(NextRestYaw(45), 225));    // at rest: a half turn on, not none
  CHECK(Close(NextRestYaw(100), 225));   // 125 degrees to go: enough
  CHECK(Close(NextRestYaw(200), 405));   // 25 to go is too little to see: the next one
  CHECK(Close(NextRestYaw(-30), 45));
  for (float yaw = -400; yaw < 400; yaw += 7) {
    const float rest = NextRestYaw(yaw);
    CHECK(rest - yaw >= 30 - 0.01f && rest - yaw < 210 + 0.01f);  // always forward, never more than a half turn and a bit
    // Only 45 + 180k: the box shows the same faces as at rest, never the other pair (a mirror image).
    CHECK(Close(std::fmod(std::fmod(rest - 45, 180.0f) + 180, 180.0f), 0) ||
          Close(std::fmod(std::fmod(rest - 45, 180.0f) + 180, 180.0f), 180));
  }
}

void TestIdle() {
  for (const Display display : {Display::Off, Display::On, Display::Starting, Display::Error}) {
    const CubePose rest = CubePoseFor(display);
    float highest = 0;
    for (double s = 0; s < 120; s += 0.37) {
      const CubePose p = Idle(rest, display, s);
      CHECK(p.lift >= rest.lift - 0.01f && p.lift <= rest.lift + 1.41f);  // floats up a little, never sinks
      CHECK(std::abs(p.yaw - rest.yaw) <= 2.01f && std::abs(p.pitch - rest.pitch) <= 0.81f);  // the same faces show
      CHECK(p.press >= 0 && p.press <= 1 && p.glow <= rest.glow + 0.001f && p.glow >= rest.glow * 0.87f);
      highest = std::max(highest, p.lift - rest.lift);
    }
    CHECK(highest > 1.0f);  // and it does move
  }
  const CubePose down = CubePoseFor(Display::ServiceDown);
  const CubePose still = Idle(down, Display::ServiceDown, 3.3);
  CHECK(Close(still.lift, down.lift) && Close(still.yaw, down.yaw) && Close(still.press, down.press));  // dead stays dead
  const CubePose connecting = Idle(CubePoseFor(Display::Starting), Display::Starting, 2.0);
  CHECK(Close(connecting.yaw, CubePoseFor(Display::Starting).yaw));  // the spin is its motion: no sway on it
}

void TestBlend() {
  const CubePose off = CubePoseFor(Display::Off);
  const CubePose on = CubePoseFor(Display::On);
  CHECK(Close(BlendPose(off, on, 0).lift, off.lift) && Close(BlendPose(off, on, 1).lift, on.lift));
  CHECK(Close(BlendPose(off, on, 2).glow, on.glow));  // past the end: the end
  const CubePose mid = BlendPose(off, on, 0.5f);
  CHECK(mid.lift > off.lift && mid.lift < on.lift && Close(mid.body.r, (off.body.r + on.body.r) / 2));
  CHECK(Close(EaseOut(0), 0) && Close(EaseOut(1), 1) && EaseOut(0.5f) > 0.5f);  // fast first, settling
}

void TestPoses() {
  // On floats and glows; off rests dark; an error is askew; connecting lifts a little.
  const CubePose on = CubePoseFor(Display::On);
  const CubePose off = CubePoseFor(Display::Off);
  CHECK(on.lift > 0 && on.glow > off.glow && off.lift == 0 && off.glow == 0);
  // The same dark glass in every state; the one colour is the core's.
  CHECK(Close(on.body.r, off.body.r) && Close(on.body.b, off.body.b));
  CHECK(on.rim.a > off.rim.a);
  CHECK(on.signal.b > on.signal.r && CubePoseFor(Display::Error).signal.r > CubePoseFor(Display::Error).signal.b);
  CHECK(!Close(std::fmod(CubePoseFor(Display::Error).yaw - 45, 90.0f), 0));
  CHECK(CubePoseFor(Display::Starting).lift > 0);
  // The core: an ember while off, full while on, between while it connects.
  CHECK(off.press == 0 && on.press == 1);
  const float connecting = CubePoseFor(Display::Starting).press;
  CHECK(connecting > 0.2f && connecting < 0.8f);
  CHECK(Close(BlendPose(off, on, 0.5f).press, 0.5f));
  // See-through glass.
  CHECK(on.body.a < 0.8f);
}

void TestFaces() {
  // Square-on to a corner and tipped: two sides and the top.
  const CubePose pose = CubePoseFor(Display::Off);
  const auto faces = CubeFaces(pose, {50, 50}, 20);
  CHECK(faces.size() == 3);
  CHECK(std::count_if(faces.begin(), faces.end(), [](const CubeFace& f) { return f.top; }) == 1);
  // The light is up and left: the top brightest, the left side over the right.
  float top = 0;
  float left = 0;
  float right = 0;
  for (const CubeFace& f : faces) {
    float x = 0;
    for (const auto& c : f.corners) {
      x += c.x / 4;
    }
    (f.top ? top : x < 50 ? left : right) = f.shade;
  }
  CHECK(top > left && left > right);
  // Face-on to a side: the side and the top only - an edge-on face isn't drawn.
  CubePose square = pose;
  square.yaw = 0;
  CHECK(CubeFaces(square, {50, 50}, 20).size() == 2);
  // Lift moves it up the screen, nothing else.
  CubePose lifted = pose;
  lifted.lift = 10;
  const auto up = CubeFaces(lifted, {50, 50}, 20);
  CHECK(up.size() == faces.size() && Close(up[0].corners[0].y, faces[0].corners[0].y - 10) &&
        Close(up[0].corners[0].x, faces[0].corners[0].x));
  // Every corner within reach of the center: half-edge times the half-diagonal (sqrt 3), plus the lift.
  for (const CubeFace& f : up) {
    for (const auto& c : f.corners) {
      CHECK(std::hypot(c.x - 50, c.y - 40) <= 20 * std::sqrt(3.0f) + 0.01f);
    }
  }
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestRestYaw();
    TestBlend();
  TestIdle();
    TestPoses();
    TestFaces();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
