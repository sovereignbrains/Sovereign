package main

// The C trampoline lives in its own file: cgo copies the preamble of a file
// that has //export directives into two generated C files, so such a
// preamble may only hold declarations. This file has no exports, so it may
// define the (static) function that actually calls the host's pointer — Go
// cannot call a C function pointer directly.

/*
#include <stdlib.h>

typedef void (*sovereign_log_fn)(void* ctx, int level, const char* message);

static void sovereign_call_log(sovereign_log_fn fn, void* ctx, int level, const char* message) {
	fn(ctx, level, message);
}
*/
import "C"

import (
	"regexp"
	"sync"
	"unsafe"

	"github.com/sagernet/sing-box/log"
)

// logMu is an RWMutex rather than a plain mutex for one guarantee the host
// relies on: every callback runs under RLock, box_set_log_callback swaps
// under Lock, so once box_set_log_callback returns no call with the previous
// callback/context is still in flight or can start. The C++ side frees the
// object behind ctx right after unregistering; without this that would be a
// use-after-free racing a log line from a sing-box goroutine.
var (
	logMu  sync.RWMutex
	logFn  C.sovereign_log_fn
	logCtx unsafe.Pointer
)

func setLogCallback(fn C.sovereign_log_fn, ctx unsafe.Pointer) {
	logMu.Lock()
	defer logMu.Unlock()
	logFn = fn
	logCtx = ctx
}

// sing-box's platform formatter keeps its ANSI level colours (the switch to
// turn them off is commented out upstream, log/observable.go); a host log
// view wants plain text.
var ansiEscape = regexp.MustCompile("\x1b\\[[0-9;]*m")

// platformWriter is sing-box's own hook for embedding hosts (log.PlatformWriter,
// the one its mobile clients use), attached to the running box's log factory.
type platformWriter struct {
	factory log.Factory
}

func (w platformWriter) WriteMessage(level log.Level, message string) {
	// Platform writers receive every level, whatever log.level says (see
	// output() in sing-box log/observable.go) — filter here so the host sees
	// exactly what `sing-box run` with the same config would print.
	if level > w.factory.Level() {
		return
	}
	logMu.RLock()
	defer logMu.RUnlock()
	if logFn == nil {
		return
	}
	cMessage := C.CString(ansiEscape.ReplaceAllString(message, ""))
	defer C.free(unsafe.Pointer(cMessage))
	C.sovereign_call_log(logFn, logCtx, C.int(level), cMessage)
}
