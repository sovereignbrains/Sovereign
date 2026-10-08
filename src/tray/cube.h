#pragma once

#include <d2d1.h>

#include <array>
#include <vector>

#include "tray_model.h"

// The connect button as a cube of matte glass (main screen, the plan of
// 04.10): its pose says the state - resting grey while off, turning while
// it connects, lifted and lit from inside while on, knocked askew on an
// error. A change of state is a quarter turn into the new pose; nothing
// moves otherwise (no timer, no CPU), and with Windows' animations off it
// takes the pose at once.
//
// Drawn with Direct2D, no 3D engine: an orthographic view keeps a cube's
// faces parallelograms, so each is a filled path, and a circle on the top
// face an ellipse. The lever turns in the plane facing the viewer, so it
// reads the same whichever way the cube has turned. The pose math is pure,
// tested in tests/unit/cube_test.cpp.

namespace sovereign::tray {

struct CubePose {
  float yaw = 45;    // degrees round the vertical; 45 + 90k shows it square-on to a corner
  float pitch = 26;  // degrees the top tips toward the viewer
  float lift = 0;    // DIPs above where it rests
  float glow = 0;    // 0..1: the light inside, and around it
  float press = 0;   // 0..1: the lever on top, thrown left (off) to right (on)
  D2D1_COLOR_F body{};    // the glass
  D2D1_COLOR_F rim{};     // the edges' light
  D2D1_COLOR_F signal{};  // the lever's knob: the one colour, the state's
};

// The pose a state rests in.
CubePose CubePoseFor(Display display);

// Between `a` (t = 0) and `b` (t = 1).
CubePose BlendPose(const CubePose& a, const CubePose& b, float t);

// Fast at first, settling at the end: t in 0..1.
float EaseOut(float t);

// Where a turn from `yaw` comes to rest: the next 45 + 90k at least 30
// degrees on - always forward, always a visible turn.
float NextRestYaw(float yaw);

// A face that shows: its corners on screen in order round it, how lit it
// is (0..1, from a light up, left and in front), and whether it's the top.
struct CubeFace {
  std::array<D2D1_POINT_2F, 4> corners{};
  float shade = 0;
  bool top = false;
};

// The faces of a cube with half-edge `half` at `pose`, centered on `center`
// (lift moves it up), that face the viewer.
std::vector<CubeFace> CubeFaces(const CubePose& pose, D2D1_POINT_2F center, float half);

// The cube in `box` (square), with its shadow and glow, and a knife switch on
// its top face: a lever in a slotted mount, thrown left while off, right
// while on, upright while it connects. Hovered nudges the lever toward the
// other side, pressed throws it further.
void DrawCube(ID2D1RenderTarget* target, const CubePose& pose, D2D1_RECT_F box, bool hovered, bool pressed);

// The lever's angle off upright, degrees (negative: to the left) for a press.
float LeverAngle(float press);

// The main screen's night: a wireframe land of rolling hills running off to
// a horizon, rising toward the window's sides; two ranges of mountains on
// the horizon, their ridges catching the light; a glow at the horizon's
// middle lighting the land nearest it; mist and stars in the sky. All in the
// cube's greys, no colour; the same every time for a window of a size.
void DrawNight(ID2D1RenderTarget* target, D2D1_SIZE_F size);

}  // namespace sovereign::tray
