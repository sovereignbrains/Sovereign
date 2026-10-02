// Flag emoji in server names (src/tray/flags.h): found where they are, as
// the sprite's flags, and nothing else taken for one.

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "check.h"
#include "flags.h"

namespace {

using namespace sovereign::tray;

// A flag emoji: two regional indicators, as UTF-16.
std::wstring Flag(char a, char b) {
  return {wchar_t{0xD83C}, static_cast<wchar_t>(0xDDE6 + (a - 'A')), wchar_t{0xD83C},
          static_cast<wchar_t>(0xDDE6 + (b - 'A'))};
}

void TestIndex() {
  CHECK(FlagIndex("ad") == 0u);
  CHECK(FlagIndex("NL").has_value() && FlagIndex("NL") == FlagIndex("nl"));
  CHECK(FlagIndex("eu").has_value());
  CHECK(!FlagIndex("zz").has_value());
  CHECK(!FlagIndex("n").has_value());
}

void TestFind() {
  // "🇪🇺 4G | Whitelist №1" - as the subscription names it.
  const std::wstring eu = Flag('E', 'U') + L" 4G | Whitelist №1";
  const auto found = FindFlagEmoji(eu);
  CHECK(found.size() == 1);
  if (found.size() == 1) {
    CHECK(found[0].at == 0 && found[0].index == FlagIndex("eu"));
  }

  // In the middle and twice, and a name without one.
  const std::wstring two = L"auto: " + Flag('D', 'E') + L" Frankfurt, " + Flag('N', 'L');
  const auto both = FindFlagEmoji(two);
  CHECK(both.size() == 2);
  if (both.size() == 2) {
    CHECK(both[0].at == 6 && both[0].index == FlagIndex("de"));
    CHECK(both[1].at == two.size() - 4 && both[1].index == FlagIndex("nl"));
  }
  CHECK(FindFlagEmoji(L"AnyTLS-REALITY").empty());
  CHECK(FindFlagEmoji(L"").empty());

  // No such flag ("ZZ"): left alone. A stray indicator before a real pair
  // doesn't swallow it.
  CHECK(FindFlagEmoji(Flag('Z', 'Z') + L" x").empty());
  const std::wstring stray = Flag('X', 'N').substr(0, 2) + Flag('N', 'L');
  const auto shifted = FindFlagEmoji(stray);
  CHECK(shifted.size() == 1 && shifted[0].at == 2 && shifted[0].index == FlagIndex("nl"));

  // Half a pair at the end, other emoji (a surrogate pair, not an indicator).
  CHECK(FindFlagEmoji(Flag('D', 'E').substr(0, 3)).empty());
  CHECK(FindFlagEmoji(L"\xD83D\xDE80 rocket").empty());
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestIndex();
    TestFind();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
