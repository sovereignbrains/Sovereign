#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// Mirrors github.com/sagernet/sing/common/json/badoption.Duration: a
// time.Duration marshaled as Go's time.Duration.String() and parsed with
// sing's my_time.ParseDuration (Go's time.ParseDuration plus a "d" unit for
// days). Both are ports of the Go code, integer arithmetic included - an
// earlier approximation printed 500ms as "0.5s" and lost digits of long
// fractions, which the golden test with sub-second durations caught.

namespace sovereign::adapters {

class Duration {
 public:
  Duration() = default;
  explicit Duration(std::chrono::nanoseconds value) : value_(value) {}

  std::chrono::nanoseconds value() const { return value_; }

 private:
  std::chrono::nanoseconds value_{};
};

inline bool IsEmptyValue(const Duration& d) { return d.value().count() == 0; }

// time.Duration.String(): "72h3m0.5s", "1.5s", "500ms", "1.1µs", "2ns",
// "0s". Built back to front, like the Go code.
inline std::string FormatDuration(std::chrono::nanoseconds ns) {
  constexpr std::uint64_t kSecond = 1'000'000'000;
  constexpr std::uint64_t kMillisecond = 1'000'000;
  constexpr std::uint64_t kMicrosecond = 1'000;
  const std::int64_t d = ns.count();
  // |d| without overflow for the most negative value.
  std::uint64_t u = d < 0 ? ~static_cast<std::uint64_t>(d) + 1 : static_cast<std::uint64_t>(d);
  if (u == 0) {
    return "0s";
  }
  std::string reversed;
  // v's last `prec` digits as a fraction, trailing zeros (and a bare point)
  // omitted; returns v with them removed.
  const auto fraction = [&](std::uint64_t v, int prec) {
    bool print = false;
    for (int i = 0; i < prec; ++i) {
      const auto digit = static_cast<char>(v % 10);
      print = print || digit != 0;
      if (print) {
        reversed += static_cast<char>('0' + digit);
      }
      v /= 10;
    }
    if (print) {
      reversed += '.';
    }
    return v;
  };
  const auto integer = [&](std::uint64_t v) {
    do {
      reversed += static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v > 0);
  };

  reversed += 's';
  if (u < kSecond) {
    int prec = 0;
    if (u < kMicrosecond) {
      reversed += 'n';
    } else if (u < kMillisecond) {
      prec = 3;
      reversed += "\xB5\xC2";  // "µ" (U+00B5, UTF-8 C2 B5), reversed
    } else {
      prec = 6;
      reversed += 'm';
    }
    u = fraction(u, prec);
    integer(u);
  } else {
    u = fraction(u, 9);
    integer(u % 60);
    u /= 60;
    if (u > 0) {
      reversed += 'm';
      integer(u % 60);
      u /= 60;
      if (u > 0) {
        reversed += 'h';
        integer(u);
      }
    }
  }
  if (d < 0) {
    reversed += '-';
  }
  return {reversed.rbegin(), reversed.rend()};
}

// my_time.ParseDuration: [-+]?([0-9]*(\.[0-9]*)?[a-z]+)+ with units ns, us,
// µs, μs, ms, s, m, h, d, or a bare "0". Throws std::invalid_argument with
// Go's message on anything else - callers go through nlohmann's exception
// path, like every other adapter here.
inline std::chrono::nanoseconds ParseDuration(const std::string& text) {
  constexpr std::uint64_t kLimit = std::uint64_t{1} << 63U;
  const auto invalid = [&] { return std::invalid_argument("time: invalid duration \"" + text + "\""); };
  std::string_view s = text;
  bool negative = false;
  if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
    negative = s.front() == '-';
    s.remove_prefix(1);
  }
  if (s == "0") {
    return std::chrono::nanoseconds{0};
  }
  if (s.empty()) {
    throw invalid();
  }
  const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };

  std::uint64_t total = 0;
  while (!s.empty()) {
    if (!(s.front() == '.' || isDigit(s.front()))) {
      throw invalid();
    }
    // [0-9]*
    std::uint64_t v = 0;
    std::size_t i = 0;
    for (; i < s.size() && isDigit(s[i]); ++i) {
      if (v > kLimit / 10) {
        throw invalid();
      }
      v = v * 10 + static_cast<std::uint64_t>(s[i] - '0');
      if (v > kLimit) {
        throw invalid();
      }
    }
    const bool pre = i > 0;
    s.remove_prefix(i);

    // (\.[0-9]*)? - precision just stops accumulating on overflow
    std::uint64_t f = 0;
    double scale = 1;
    bool post = false;
    if (!s.empty() && s.front() == '.') {
      s.remove_prefix(1);
      bool overflow = false;
      std::size_t j = 0;
      for (; j < s.size() && isDigit(s[j]); ++j) {
        if (overflow) {
          continue;
        }
        if (f > (kLimit - 1) / 10) {
          overflow = true;
          continue;
        }
        const std::uint64_t y = f * 10 + static_cast<std::uint64_t>(s[j] - '0');
        if (y > kLimit) {
          overflow = true;
          continue;
        }
        f = y;
        scale *= 10;
      }
      post = j > 0;
      s.remove_prefix(j);
    }
    if (!pre && !post) {
      throw invalid();
    }

    // unit: everything up to the next number
    std::size_t k = 0;
    while (k < s.size() && !(s[k] == '.' || isDigit(s[k]))) {
      ++k;
    }
    if (k == 0) {
      throw std::invalid_argument("time: missing unit in duration \"" + text + "\"");
    }
    const std::string_view unitName = s.substr(0, k);
    s.remove_prefix(k);
    std::uint64_t unit = 0;
    if (unitName == "ns") {
      unit = 1;
    } else if (unitName == "us" || unitName == "\xC2\xB5s" || unitName == "\xCE\xBCs") {  // µs, μs
      unit = 1'000;
    } else if (unitName == "ms") {
      unit = 1'000'000;
    } else if (unitName == "s") {
      unit = 1'000'000'000;
    } else if (unitName == "m") {
      unit = 60 * std::uint64_t{1'000'000'000};
    } else if (unitName == "h") {
      unit = 3600 * std::uint64_t{1'000'000'000};
    } else if (unitName == "d") {
      unit = 24 * 3600 * std::uint64_t{1'000'000'000};
    } else {
      throw std::invalid_argument("time: unknown unit \"" + std::string(unitName) + "\" in duration \"" + text + "\"");
    }
    if (v > kLimit / unit) {
      throw invalid();
    }
    v *= unit;
    if (f > 0) {
      // As in Go: float64 keeps fractions of hours nanosecond-accurate.
      v += static_cast<std::uint64_t>(static_cast<double>(f) * (static_cast<double>(unit) / scale));
      if (v > kLimit) {
        throw invalid();
      }
    }
    total += v;
    if (total > kLimit) {
      throw invalid();
    }
  }
  if (negative) {
    return std::chrono::nanoseconds{static_cast<std::int64_t>(~total + 1)};
  }
  if (total > kLimit - 1) {
    throw invalid();
  }
  return std::chrono::nanoseconds{static_cast<std::int64_t>(total)};
}

inline void to_json(nlohmann::json& j, const Duration& d) { j = FormatDuration(d.value()); }

inline void from_json(const nlohmann::json& j, Duration& d) { d = Duration(ParseDuration(j.get<std::string>())); }

}  // namespace sovereign::adapters
