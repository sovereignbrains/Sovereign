#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

// The country flags sprite (assets/flags/flags.png, resource IDR_FLAGS): 257
// flags of kFlagWidth x kFlagHeight pixels, kFlagColumns a row, in the order
// of kFlagCodes. assets/flags/README.md says how it's made.

namespace sovereign::tray {

inline constexpr int kFlagWidth = 48;
inline constexpr int kFlagHeight = 36;
inline constexpr int kFlagColumns = 16;

// Lowercase ISO 3166-1 alpha-2 codes (and a few like "eu" and "xx", the
// unknown flag), space-separated.
inline constexpr std::string_view kFlagCodes =
    "ad ae af ag ai al am ao aq ar as at au aw ax az ba bb bd be bf bg bh bi bj bl bm bn bo bq br bs bt bv bw "
    "by bz ca cc cd cf cg ch ci ck cl cm cn co cp cr cu cv cw cx cy cz de dg dj dk dm do dz ec ee eg eh er es "
    "et eu fi fj fk fm fo fr ga gb gd ge gf gg gh gi gl gm gn gp gq gr gs gt gu gw gy hk hm hn hr ht hu ic id "
    "ie il im in io iq ir is it je jm jo jp ke kg kh ki km kn kp kr kw ky kz la lb lc li lk lr ls lt lu lv ly "
    "ma mc md me mf mg mh mk ml mm mn mo mp mq mr ms mt mu mv mw mx my mz na nc ne nf ng ni nl no np nr nu nz "
    "om pa pc pe pf pg ph pk pl pm pn pr ps pt pw py qa re ro rs ru rw sa sb sc sd se sg sh si sj sk sl sm sn "
    "so sr ss st sv sx sy sz tc td tf tg th tj tk tl tm tn to tr tt tv tw tz ua ug um un us uy uz va vc ve vg "
    "vi vn vu wf ws xk xx ye yt za zm zw";

// The index of a code's flag in the sprite ("NL" or "nl"); nullopt if none.
inline std::optional<std::size_t> FlagIndex(std::string_view code) {
  if (code.size() != 2) {
    return std::nullopt;
  }
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
  for (std::size_t i = 0; i + 1 < kFlagCodes.size(); i += 3) {
    if (kFlagCodes[i] == lower(code[0]) && kFlagCodes[i + 1] == lower(code[1])) {
      return i / 3;
    }
  }
  return std::nullopt;
}

}  // namespace sovereign::tray
