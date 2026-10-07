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
  CHECK(Close(NextRestYaw(45), 135));    // square-on: a full quarter turn on, not none
  CHECK(Close(NextRestYaw(100), 135));   // 35 degrees to go: enough
  CHECK(Close(NextRestYaw(120), 225));   // 15 to go is too little to see: the next one
  CHECK(Close(NextRestYaw(-30), 45));
  for (float yaw = -400; yaw < 400; yaw += 7) {
    const float rest = NextRestYaw(yaw);
    CHECK(rest - yaw >= 30 - 0.01f && rest - yaw < 120 + 0.01f);  // always forward, never more than a turn and a bit
    CHECK(Close(std::fmod(std::fmod(rest - 45, 90.0f) + 90, 90.0f), 0) ||
          Close(std::fmod(std::fmod(rest - 45, 90.0f) + 90, 90.0f), 90));
  }
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
  // The same dark glass in every state; the one colour is the knob's.
  CHECK(Close(on.body.r, off.body.r) && Close(on.body.b, off.body.b));
  CHECK(on.rim.a > off.rim.a);
  CHECK(on.signal.b > on.signal.r && CubePoseFor(Display::Error).signal.r > CubePoseFor(Display::Error).signal.b);
  CHECK(!Close(std::fmod(CubePoseFor(Display::Error).yaw - 45, 90.0f), 0));
  CHECK(CubePoseFor(Display::Starting).lift > 0);
  // The lever: thrown left while off, right while on, about upright while it connects.
  CHECK(off.press == 0 && on.press == 1);
  CHECK(LeverAngle(off.press) < -20 && LeverAngle(on.press) > 20 && Close(LeverAngle(0), -LeverAngle(1)));
  const float connecting = LeverAngle(CubePoseFor(Display::Starting).press);
  CHECK(std::abs(connecting) < 10);
  CHECK(Close(BlendPose(off, on, 0.5f).press, 0.5f) && Close(LeverAngle(0.5f), 0));
  CHECK(Close(LeverAngle(2), LeverAngle(1)));  // past the end: thrown all the way, no further
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
    TestPoses();
    TestFaces();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
