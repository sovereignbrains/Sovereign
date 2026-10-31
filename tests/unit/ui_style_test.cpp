// The tray's look against its law (docs/design/law.md): ui_style.h carries the
// same numbers as docs/design/tokens.css, every text pair passes AA, the four
// fates tell apart, the type scale keeps its step, and no .cpp of the tray
// sets a hue of its own.

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#include "check.h"
#include "ui_style.h"

namespace {

using namespace sovereign::tray;
namespace fs = std::filesystem;

struct Rgb3 {
  double r, g, b;
};

Rgb3 Of(COLORREF c) { return {static_cast<double>(GetRValue(c)), static_cast<double>(GetGValue(c)), static_cast<double>(GetBValue(c))}; }

double Lin(double v) {
  const double c = v / 255;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double Luminance(Rgb3 c) { return 0.2126 * Lin(c.r) + 0.7152 * Lin(c.g) + 0.0722 * Lin(c.b); }

double Ratio(COLORREF x, COLORREF y) {
  const double a = Luminance(Of(x));
  const double b = Luminance(Of(y));
  return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

// CIE76 on D65: the same maths as Law.color.dE of the mockup.
void Lab(Rgb3 c, double out[3]) {
  const double r = Lin(c.r);
  const double g = Lin(c.g);
  const double b = Lin(c.b);
  const double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
  const double y = 0.2126729 * r + 0.7151522 * g + 0.072175 * b;
  const double z = (0.0193339 * r + 0.119192 * g + 0.9503041 * b) / 1.08883;
  const auto f = [](double t) { return t > 216.0 / 24389 ? std::cbrt(t) : ((24389.0 / 27) * t + 16) / 116; };
  out[0] = 116 * f(y) - 16;
  out[1] = 500 * (f(x) - f(y));
  out[2] = 200 * (f(y) - f(z));
}

double DeltaE(COLORREF x, COLORREF y) {
  double p[3];
  double q[3];
  Lab(Of(x), p);
  Lab(Of(y), q);
  return std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
}

std::string Slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void TestTextPairs() {
  using namespace ui;
  for (const COLORREF ink : {kPrimaryText, kSecondaryText, kVpn, kWarp, kDirect, kBlock, kUpload, kWarning, kLit}) {
    CHECK(Ratio(ink, kCardColor) >= 4.5);
  }
  // The secondary text sits on every surface there is.
  for (const COLORREF surface : {kWindowColor, kPanelColor, kCardColor, kDialogColor, kRaisedColor}) {
    CHECK(Ratio(kSecondaryText, surface) >= 4.5);
    CHECK(Ratio(kPrimaryText, surface) >= 7.0);
  }
  CHECK(Ratio(kLitInk, kLit) >= 4.5);  // text on the hand's fill
}

void TestFatesTellApart() {
  using namespace ui;
  const COLORREF fates[] = {kVpn, kWarp, kDirect, kBlock};
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = i + 1; j < 4; ++j) {
      CHECK(DeltaE(fates[i], fates[j]) >= 30.0);
    }
  }
  CHECK(DeltaE(kWarning, kWarp) >= 30.0);   // attention is not WARP
  CHECK(DeltaE(kWarning, kBlock) >= 30.0);  // and not blocked
}

void TestTypeScale() {
  using namespace ui;
  const float scale[] = {kTMicro, kTCaption, kTBody, kTHeading, kTLead, kTTitle};
  for (size_t i = 1; i < std::size(scale); ++i) {
    const double step = scale[i] / scale[i - 1];
    CHECK(step >= 1.11 && step <= 1.14);
  }
}

// --name-rgb: R G B;  ->  name -> COLORREF
std::map<std::string, COLORREF> TokenColors(const std::string& css) {
  std::map<std::string, COLORREF> out;
  static const std::regex re(R"(--([a-z0-9-]+)-rgb:\s*(\d+)\s+(\d+)\s+(\d+)\s*;)");
  for (std::sregex_iterator it(css.begin(), css.end(), re), end; it != end; ++it) {
    const auto& m = *it;
    out[m[1].str()] = RGB(std::stoi(m[2].str()), std::stoi(m[3].str()), std::stoi(m[4].str()));
  }
  return out;
}

void TestTokensCss() {
  using namespace ui;
  const std::string css = Slurp(fs::path(SOV_REPO_DIR) / "docs" / "design" / "tokens.css");
  CHECK(!css.empty());
  const auto tokens = TokenColors(css);
  const std::pair<const char*, COLORREF> expected[] = {
      {"window", kWindowColor}, {"panel", kPanelColor}, {"card", kCardColor},   {"dialog", kDialogColor},
      {"raised", kRaisedColor}, {"ink-1", kPrimaryText}, {"ink-2", kSecondaryText}, {"lit", kLit},
      {"lit-ink", kLitInk},     {"vpn", kVpn},           {"warp", kWarp},       {"direct", kDirect},
      {"block", kBlock},        {"ok", kUpload},         {"warn", kWarning},
  };
  for (const auto& [name, value] : expected) {
    const auto found = tokens.find(name);
    if (found == tokens.end()) {
      std::cerr << "tokens.css has no --" << name << "-rgb\n";
      CHECK(false);
      continue;
    }
    if (found->second != value) {
      std::cerr << "--" << name << "-rgb differs from ui_style.h\n";
    }
    CHECK(found->second == value);
  }

  // The type scale: --t-*: Npx.
  const std::pair<const char*, float> sizes[] = {{"micro", kTMicro}, {"caption", kTCaption}, {"body", kTBody},
                                                 {"heading", kTHeading}, {"lead", kTLead},   {"title", kTTitle}};
  for (const auto& [name, value] : sizes) {
    const std::regex re(std::string("--t-") + name + R"(:\s*([0-9.]+)px)");
    std::smatch m;
    const bool found = std::regex_search(css, m, re);
    CHECK(found);
    if (found) {
      CHECK(std::abs(std::stof(m[1].str()) - value) < 0.001f);
    }
  }
}

// The law: a hue is the fate of a flow or a verdict, and it is named once, in
// ui_style.h. A literal with colour in it anywhere else is a second meaning.
// Greys (a channel spread up to 12: the paper's tints) and the alpha-only
// whites and blacks are not hues.
void TestNoHueLiterals() {
  static const std::regex re(R"(\b(?:RGB|Rgb)\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+))");
  int scanned = 0;
  for (const auto& entry : fs::directory_iterator(fs::path(SOV_REPO_DIR) / "src" / "tray")) {
    if (entry.path().extension() != ".cpp") {
      continue;
    }
    ++scanned;
    std::istringstream lines(Slurp(entry.path()));
    std::string line;
    int number = 0;
    while (std::getline(lines, line)) {
      ++number;
      for (std::sregex_iterator it(line.begin(), line.end(), re), end; it != end; ++it) {
        const int c[] = {std::stoi((*it)[1].str()), std::stoi((*it)[2].str()), std::stoi((*it)[3].str())};
        const int spread = *std::max_element(c, c + 3) - *std::min_element(c, c + 3);
        if (spread > 12) {
          std::cerr << entry.path().filename().string() << ":" << number << ": a hue outside ui_style.h: " << line << "\n";
          CHECK(false);
        }
      }
    }
  }
  CHECK(scanned > 10);  // the scan found the sources at all
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestTextPairs();
    TestFatesTellApart();
    TestTypeScale();
    TestTokensCss();
    TestNoHueLiterals();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
