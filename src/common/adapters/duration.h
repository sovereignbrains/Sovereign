#pragma once

#include <cctype>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

// Mirrors github.com/sagernet/sing/common/json/badoption.Duration: a
// time.Duration marshaled as Go's duration string (e.g. "1h30m0s"),
// parsed leniently on the way in (my_time.ParseDuration accepts a
// superset of Go's own format — we only need to accept what our own
// to_json below ever produces, plus plain "<n>s"/"<n>ms"/"<n>h"/"<n>m"
// forms, which covers every duration field in the option package today).

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

// Formats like time.Duration.String(): largest whole unit among h/m/s,
// with fractional seconds trimmed of trailing zeros. Good enough for the
// durations sing-box's option schema actually uses (timeouts, keepalive
// intervals) — all >= 1ms, none needing the sub-millisecond units Go's
// formatter also supports.
inline std::string FormatDuration(std::chrono::nanoseconds ns) {
  using namespace std::chrono;
  if (ns.count() == 0) {
    return "0s";
  }
  std::string sign;
  auto abs = ns;
  if (abs.count() < 0) {
    sign = "-";
    abs = -abs;
  }
  const auto h = duration_cast<hours>(abs);
  abs -= h;
  const auto m = duration_cast<minutes>(abs);
  abs -= m;
  const double seconds = duration_cast<duration<double>>(abs).count();

  char buf[64];
  std::string out = sign;
  if (h.count() != 0) {
    out += std::to_string(h.count()) + "h";
  }
  if (h.count() != 0 || m.count() != 0) {
    out += std::to_string(m.count()) + "m";
  }
  std::snprintf(buf, sizeof(buf), "%g", seconds);
  out += std::string(buf) + "s";
  return out;
}

// Parses "1h30m5.5s"-style strings plus bare "<n><unit>" (unit one of
// ns/us/ms/s/m/h). Throws std::invalid_argument on anything else — callers
// go through nlohmann's exception path, matching every other adapter here.
inline std::chrono::nanoseconds ParseDuration(const std::string& text) {
  using namespace std::chrono;
  if (text.empty()) {
    throw std::invalid_argument("empty duration");
  }
  size_t i = 0;
  bool negative = false;
  if (text[i] == '+' || text[i] == '-') {
    negative = text[i] == '-';
    ++i;
  }
  if (text.substr(i) == "0") {
    return nanoseconds{0};
  }

  nanoseconds total{0};
  bool sawComponent = false;
  while (i < text.size()) {
    const size_t numStart = i;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.')) {
      ++i;
    }
    if (i == numStart) {
      throw std::invalid_argument("bad duration: " + text);
    }
    const double amount = std::stod(text.substr(numStart, i - numStart));

    const size_t unitStart = i;
    while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) {
      ++i;
    }
    const std::string unit = text.substr(unitStart, i - unitStart);

    if (unit == "ns") {
      total += nanoseconds(static_cast<long long>(amount));
    } else if (unit == "us" || unit == "\xC2\xB5s") {
      total += duration_cast<nanoseconds>(duration<double, std::micro>(amount));
    } else if (unit == "ms") {
      total += duration_cast<nanoseconds>(duration<double, std::milli>(amount));
    } else if (unit == "s") {
      total += duration_cast<nanoseconds>(duration<double>(amount));
    } else if (unit == "m") {
      total += duration_cast<nanoseconds>(duration<double, std::ratio<60>>(amount));
    } else if (unit == "h") {
      total += duration_cast<nanoseconds>(duration<double, std::ratio<3600>>(amount));
    } else {
      throw std::invalid_argument("unknown duration unit: " + unit);
    }
    sawComponent = true;
  }
  if (!sawComponent) {
    throw std::invalid_argument("bad duration: " + text);
  }
  return negative ? -total : total;
}

inline void to_json(nlohmann::json& j, const Duration& d) {
  j = FormatDuration(d.value());
}

inline void from_json(const nlohmann::json& j, Duration& d) {
  d = Duration(ParseDuration(j.get<std::string>()));
}

}  // namespace sovereign::adapters
