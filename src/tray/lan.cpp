#include "lan.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <tuple>
#include <utility>

namespace sovereign::tray {

namespace {

// "192.168.31.7" as a number; nullopt unless four plain decimal parts.
std::optional<std::uint32_t> ParseV4(std::string_view text) {
  std::uint32_t value = 0;
  std::size_t at = 0;
  for (int part = 0; part < 4; ++part) {
    if (part > 0) {
      if (at >= text.size() || text[at] != '.') {
        return std::nullopt;
      }
      ++at;
    }
    const std::size_t start = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      ++at;
    }
    unsigned octet = 0;
    const std::size_t length = at - start;
    if (length == 0 || length > 3 || (length > 1 && text[start] == '0') ||
        std::from_chars(text.data() + start, text.data() + at, octet).ec != std::errc{} || octet > 255) {
      return std::nullopt;
    }
    value = (value << 8) | octet;
  }
  return at == text.size() ? std::optional(value) : std::nullopt;
}

std::string FormatV4(std::uint32_t v) {
  return std::format("{}.{}.{}.{}", v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// Whether all of `bits` from `v` is in the local network's ranges.
bool PrivateRange(std::uint32_t v, int bits) {
  struct Range {
    std::uint32_t base;
    int bits;
  };
  static constexpr std::array<Range, 4> kRanges = {{
      {0x0A000000, 8},   // 10/8
      {0xAC100000, 12},  // 172.16/12
      {0xC0A80000, 16},  // 192.168/16
      {0xA9FE0000, 16},  // 169.254/16
  }};
  return std::any_of(kRanges.begin(), kRanges.end(), [&](const Range& r) {
    const std::uint32_t mask = 0xFFFFFFFFU << (32 - r.bits);
    return bits >= r.bits && (v & mask) == r.base;
  });
}

// The order the window lists addresses in: by their numbers, not as text.
std::uint64_t SortKey(const std::string& address) {
  const std::string_view text(address);
  const std::size_t slash = text.find('/');
  const auto v = ParseV4(text.substr(0, slash));
  return v ? (static_cast<std::uint64_t>(*v) << 8) | (slash == std::string_view::npos ? 32 : 0) : ~std::uint64_t{0};
}

}  // namespace

std::optional<std::string> LanAddress(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  const std::size_t slash = text.find('/');
  const auto address = ParseV4(text.substr(0, slash));
  if (!address) {
    return std::nullopt;
  }
  int bits = 32;
  if (slash != std::string_view::npos) {
    const std::string_view suffix = text.substr(slash + 1);
    if (suffix.empty() || suffix.size() > 2 ||
        std::from_chars(suffix.data(), suffix.data() + suffix.size(), bits).ec != std::errc{} || bits < 8 ||
        bits > 32) {
      return std::nullopt;
    }
  }
  const std::uint32_t network = bits == 32 ? *address : *address & (0xFFFFFFFFU << (32 - bits));
  if (!PrivateRange(network, bits)) {
    return std::nullopt;
  }
  return bits == 32 ? FormatV4(network) : std::format("{}/{}", FormatV4(network), bits);
}

std::vector<LanEntry> LanEntries(const std::vector<LanDevice>& seen, const std::vector<LanHost>& allowed) {
  std::vector<LanEntry> entries;
  for (const LanDevice& device : seen) {
    if (std::any_of(entries.begin(), entries.end(), [&](const LanEntry& e) { return e.address == device.address; })) {
      continue;
    }
    LanEntry entry{
        .address = device.address, .name = {}, .mac = device.mac, .router = device.router, .allowed = false, .seen = true};
    if (const auto it = std::find_if(allowed.begin(), allowed.end(),
                                     [&](const LanHost& h) { return h.address == device.address; });
        it != allowed.end()) {
      entry.allowed = true;
      entry.name = it->name;
    }
    entries.push_back(std::move(entry));
  }
  for (const LanHost& host : allowed) {
    if (std::none_of(entries.begin(), entries.end(), [&](const LanEntry& e) { return e.address == host.address; })) {
      entries.push_back(
          {.address = host.address, .name = host.name, .mac = {}, .router = false, .allowed = true, .seen = false});
    }
  }
  std::stable_sort(entries.begin(), entries.end(), [](const LanEntry& a, const LanEntry& b) {
    return std::tuple(!a.router, !a.allowed, SortKey(a.address)) < std::tuple(!b.router, !b.allowed, SortKey(b.address));
  });
  return entries;
}

}  // namespace sovereign::tray
