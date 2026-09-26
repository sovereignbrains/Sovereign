// gocore-stub.dll: the C ABI of sovereign-gocore.dll (gocore/main.go) with no Go
// runtime behind it, so GoCore's C++ side can run under ASan.
//
// Why the real DLL can't: ASan hot-patches CreateThread process-wide and its
// interceptor unwinds the caller's stack with RtlCaptureStackBackTrace, which
// assumes the caller runs on its TEB stack. The Go runtime runs on stacks it
// allocates itself, so whenever ASLR puts such a stack below the TEB stack
// limit, _chkstk probes the guard page and the process dies with an exception
// that can't be dispatched. Diagnosis with the dump: issue #9.
//
// Behavior mirrors gocore where GoCore depends on it: every string export
// returns a fresh allocation (an empty string on success, never NULL), a second
// box_start fails until box_stop, and box_set_log_callback returns only once no
// call with the previous callback can be running or start.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

using LogCallback = void(__cdecl*)(void*, int, const char*);

// Strings come from a private heap, never from the CRT, and every one is
// counted until box_free returns it. That count - not ASan - is what catches a
// host freeing one with its own free(): MSVC's ASan hands frees of memory it
// didn't allocate to the original allocator without a report (checked by
// mutating GoCore::TakeOwnedString, 26.09.2026).
HANDLE StubHeap() {
  static const HANDLE heap = HeapCreate(0, 0, 0);
  return heap;
}

std::atomic<int> outstanding{0};

std::mutex stateMutex;  // running/generation/lastConfig
bool running = false;
std::int64_t generation = 0;
std::string lastConfig;

// Held for the whole callback invocation and for the swap - the same
// guarantee gocore gives with its RWMutex (gocore/log_bridge.go).
std::mutex logMutex;
LogCallback logCallback = nullptr;
void* logContext = nullptr;

char* Allocate(const std::string& text) {
  auto* copy = static_cast<char*>(HeapAlloc(StubHeap(), 0, text.size() + 1));
  if (copy == nullptr) {
    return nullptr;
  }
  std::memcpy(copy, text.c_str(), text.size() + 1);
  ++outstanding;
  return copy;
}

void Emit(int level, const char* message) {
  const std::scoped_lock lock(logMutex);
  if (logCallback != nullptr) {
    logCallback(logContext, level, message);
  }
}

}  // namespace

extern "C" {

// --- the gocore ABI ---------------------------------------------------------

__declspec(dllexport) char* __cdecl box_ping() { return Allocate("pong (stub)"); }

__declspec(dllexport) char* __cdecl box_start(const char* configJson) {
  const std::scoped_lock lock(stateMutex);
  if (running) {
    return Allocate("box already started; call box_stop first");
  }
  lastConfig = (configJson != nullptr) ? configJson : "";
  running = true;
  ++generation;
  return Allocate("");
}

__declspec(dllexport) char* __cdecl box_stop() {
  const std::scoped_lock lock(stateMutex);
  running = false;
  return Allocate("");
}

__declspec(dllexport) int __cdecl box_stats(std::int64_t* uplink, std::int64_t* downlink,
                                            std::int64_t* connections, std::int64_t* gen) {
  const std::scoped_lock lock(stateMutex);
  *uplink = running ? 1000 : 0;
  *downlink = running ? 2000 : 0;
  *connections = running ? 3 : 0;
  *gen = generation;
  return running ? 1 : 0;
}

__declspec(dllexport) void __cdecl box_set_log_callback(LogCallback callback, void* context) {
  const std::scoped_lock lock(logMutex);
  logCallback = callback;
  logContext = context;
}

__declspec(dllexport) void __cdecl box_free(char* text) {
  if (text != nullptr) {
    HeapFree(StubHeap(), 0, text);
    --outstanding;
  }
}

// --- test controls (not part of the gocore ABI) -------------------------------

// Strings handed out and not yet returned through box_free.
__declspec(dllexport) int __cdecl stub_outstanding() { return outstanding.load(); }

__declspec(dllexport) int __cdecl stub_has_log_callback() {
  const std::scoped_lock lock(logMutex);
  return logCallback != nullptr ? 1 : 0;
}

// Copies the config of the last successful box_start into `buffer`.
__declspec(dllexport) void __cdecl stub_last_config(char* buffer, std::size_t size) {
  const std::scoped_lock lock(stateMutex);
  if (size == 0) {
    return;
  }
  const std::size_t n = (lastConfig.size() < size - 1) ? lastConfig.size() : size - 1;
  std::memcpy(buffer, lastConfig.data(), n);
  buffer[n] = '\0';
}

// Delivers a log line on the calling thread.
__declspec(dllexport) void __cdecl stub_emit_log(int level, const char* message) {
  Emit(level, message);
}

// Delivers a log line from a thread the host didn't create - how sing-box's
// goroutines call back - and waits for it.
__declspec(dllexport) void __cdecl stub_emit_log_from_thread(int level, const char* message) {
  // Moved in, not captured as a const copy: a const member would turn the
  // closure's move into a throwing copy.
  std::string text(message);
  std::thread([level, text = std::move(text)]() noexcept {
    try {
      Emit(level, text.c_str());
    } catch (...) {
      // Only a failing mutex lock can land here - a broken test environment,
      // not a result. It can't travel back through this extern "C" export
      // (undefined behavior under /EHsc), so stop loudly instead of passing.
      std::terminate();
    }
  }).join();
}

}  // extern "C"
