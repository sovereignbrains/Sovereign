#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core.h"

namespace sovereign::service {

struct LogEntry {
  std::uint64_t seq = 0;
  LogLevel level = LogLevel::Info;
  std::string message;
};

// The core's recent log lines, kept in the service so the tray can page
// through them over the control pipe (the pipe is request/response — lines
// can't be pushed). Bounded: once full, the oldest line is dropped. Every
// line gets a sequence number that keeps increasing across drops, so a
// reader asking "what came after N" notices a gap instead of re-reading.
// Thread-safe: the core appends from its own threads while the pipe reads.
class LogRing {
 public:
  // Longer lines are cut (on a UTF-8 boundary) — one runaway line must not
  // be able to crowd everything else out of a pipe response.
  static constexpr std::size_t kMaxMessageBytes = 1024;

  explicit LogRing(std::size_t capacity);

  void Append(LogLevel level, std::string_view message);

  // Entries with seq > afterSeq, oldest first.
  std::vector<LogEntry> Since(std::uint64_t afterSeq) const;

  // Sequence number of the newest entry (0 if nothing was ever appended).
  std::uint64_t LastSeq() const;

 private:
  mutable std::mutex mutex_;
  std::deque<LogEntry> entries_;
  std::size_t capacity_;
  std::uint64_t lastSeq_ = 0;
};

// Longest prefix of `text` that is at most maxBytes long and does not end in
// the middle of a UTF-8 sequence.
std::string_view TruncateUtf8(std::string_view text, std::size_t maxBytes);

}  // namespace sovereign::service
