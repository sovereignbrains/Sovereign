#include "log_ring.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace sovereign::service {

std::string_view TruncateUtf8(std::string_view text, std::size_t maxBytes) {
  if (text.size() <= maxBytes) {
    return text;
  }
  std::size_t end = maxBytes;
  // Step back over continuation bytes (10xxxxxx) so the cut lands before the
  // lead byte of a multi-byte sequence rather than inside it.
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
    --end;
  }
  return text.substr(0, end);
}

LogRing::LogRing(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {}

void LogRing::Append(LogLevel level, std::string_view message) {
  LogEntry entry{0, level, std::string(TruncateUtf8(message, kMaxMessageBytes))};
  const std::scoped_lock lock(mutex_);
  entry.seq = ++lastSeq_;
  if (entries_.size() == capacity_) {
    entries_.pop_front();
  }
  entries_.push_back(std::move(entry));
}

std::vector<LogEntry> LogRing::Since(std::uint64_t afterSeq) const {
  const std::scoped_lock lock(mutex_);
  // Sequence numbers are contiguous inside the ring, so the first wanted
  // entry's position follows from the oldest one's seq — no search needed.
  std::vector<LogEntry> result;
  if (entries_.empty() || afterSeq >= lastSeq_) {
    return result;
  }
  const std::uint64_t firstSeq = entries_.front().seq;
  const std::size_t skip =
      afterSeq < firstSeq ? 0 : static_cast<std::size_t>(afterSeq - firstSeq + 1);
  result.assign(entries_.begin() + static_cast<std::ptrdiff_t>(skip), entries_.end());
  return result;
}

std::uint64_t LogRing::LastSeq() const {
  const std::scoped_lock lock(mutex_);
  return lastSeq_;
}

}  // namespace sovereign::service
