// Package main builds as a c-shared DLL (sovereign-gocore.dll) hosting
// sing-box inside the service process. P2 atom 1a proved the DLL loads and
// runs Go inside the C++ host (box_ping). P2 atom 1b adds box_start/box_stop:
// a real sing-box instance built from a caller-supplied JSON config, parsed
// through sing-box's own context-aware JSON machinery (include.Context wires
// up the same inbound/outbound registries the real `sing-box run` CLI uses,
// so this is not a reimplementation of config parsing — it's the real thing).
// P2 atom 2 adds box_stats (traffic counters) and box_set_log_callback, both
// through the hooks sing-box itself offers embedding hosts rather than
// anything reimplemented here.
package main

/*
#include <stdint.h>
#include <stdlib.h>

typedef void (*sovereign_log_fn)(void* ctx, int level, const char* message);
*/
import "C"

import (
	"context"
	"fmt"
	"sync"
	"unsafe"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/common/trafficcontrol"
	"github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/option"
	singjson "github.com/sagernet/sing/common/json"
)

// boxMu guards the running instance: cgo exports are just functions, there
// is no per-call object to hang state off, so the running *box.Box (if any)
// lives in package-level variables. Only one instance runs at a time —
// matches ICore's contract (the service swaps GoCore/NativeCore, not
// multiple cores at once).
var (
	boxMu         sync.Mutex
	boxInstance   *box.Box
	boxTraffic    *trafficcontrol.Manager
	boxGeneration int64
)

// box_ping proves two things at once: the DLL loads and runs Go's
// runtime inside the C++ host process, and sing-box's own package is
// actually linked in (not just cgo boilerplate) — the reply embeds
// sing-box's version constant.
//
//export box_ping
func box_ping() *C.char {
	return C.CString(fmt.Sprintf("pong (sing-box %s)", constant.Version))
}

// box_start parses configJSON (a full sing-box config document — the same
// shape `sing-box run -c` accepts, not a Sovereign-specific schema) and
// starts a sing-box instance from it. Returns an empty string on success or
// an error message on failure; either way the caller must release the
// returned string via box_free (same convention as box_ping). Starting a
// second time while an instance is already running fails with an error
// rather than silently replacing it — the caller (GoCore/the service) is
// expected to box_stop first.
//
//export box_start
func box_start(configJSON *C.char) *C.char {
	boxMu.Lock()
	defer boxMu.Unlock()

	if boxInstance != nil {
		return C.CString("box already started; call box_stop first")
	}

	ctx := include.Context(context.Background())
	var options option.Options
	if err := singjson.UnmarshalContext(ctx, []byte(C.GoString(configJSON)), &options); err != nil {
		return C.CString(fmt.Sprintf("parse config: %s", err))
	}

	instance, err := box.New(box.Options{Context: ctx, Options: options})
	if err != nil {
		return C.CString(fmt.Sprintf("create box: %s", err))
	}

	// Traffic accounting and the log hook are attached here, between New and
	// Start, instead of through box.Options.PlatformLogWriter. That option
	// would do both in one go, but it also switches on sing-box's cache file
	// (cache.db, written relative to the working directory — System32 for a
	// service), a behavior change `sing-box run` with the same config would
	// not have. Doing it by hand keeps the box exactly what the config says,
	// plus two passive observers. The manager is the same type sing-box
	// itself installs for its clash/api services; AppendTracker is how it
	// does so (box.go).
	traffic := trafficcontrol.NewManager(instance.Outbound())
	instance.Router().AppendTracker(traffic)
	instance.LogFactory().AttachPlatformWriter(platformWriter{factory: instance.LogFactory()})
	if err := traffic.Start(adapter.StartStateInitialize); err != nil {
		instance.Close()
		return C.CString(fmt.Sprintf("start traffic manager: %s", err))
	}

	if err := instance.Start(); err != nil {
		// box.Start already closes the half-started box on failure (checked:
		// a second inbound failing to bind leaves the first one's port free).
		// The traffic manager is ours, outside the box's lifecycle — close it.
		traffic.Close()
		return C.CString(fmt.Sprintf("start box: %s", err))
	}

	boxInstance = instance
	boxTraffic = traffic
	boxGeneration++
	return C.CString("")
}

// box_stop closes the running instance (if any) and clears it, so a later
// box_start can succeed. Stopping when nothing is running is not an error —
// mirrors Close() being idempotent-safe to call from a shutdown path that
// doesn't track whether Start ever succeeded.
//
//export box_stop
func box_stop() *C.char {
	boxMu.Lock()
	defer boxMu.Unlock()

	if boxInstance == nil {
		return C.CString("")
	}
	err := boxInstance.Close()
	boxTraffic.Close()
	boxInstance = nil
	boxTraffic = nil
	if err != nil {
		return C.CString(fmt.Sprintf("stop box: %s", err))
	}
	return C.CString("")
}

// box_stats reports the running instance's traffic: total bytes up/down
// (open connections plus closed ones) and how many connections are open
// right now. Plain integers through out-parameters — nothing is allocated,
// so there is no box_free obligation on a call made every UI tick.
//
// Counters belong to one box_start: they start from zero on every start.
// generation increments on every successful box_start, so a host computing
// rates from deltas can tell "counters reset" apart from "no traffic".
// Returns 1 while an instance runs, 0 otherwise (all outputs zeroed except
// generation, which keeps the last value).
//
//export box_stats
func box_stats(uplink *C.int64_t, downlink *C.int64_t, connections *C.int64_t, generation *C.int64_t) C.int {
	boxMu.Lock()
	defer boxMu.Unlock()

	*generation = C.int64_t(boxGeneration)
	if boxInstance == nil {
		*uplink, *downlink, *connections = 0, 0, 0
		return 0
	}
	up, down := boxTraffic.Total()
	*uplink = C.int64_t(up)
	*downlink = C.int64_t(down)
	*connections = C.int64_t(boxTraffic.ConnectionsLen())
	return 1
}

// box_set_log_callback registers fn to receive sing-box's log lines (plain
// text, one line per call, already filtered by the config's log.level; level
// is sing-box's log.Level: 0 panic … 6 trace). ctx is passed back untouched.
// fn runs on sing-box's own goroutines — any thread, concurrently — and
// message is only valid for the duration of the call. Pass fn = NULL to
// unregister; once this returns, the previous fn is guaranteed not to be
// running or called again (see logMu in log_bridge.go). fn must not call
// box_set_log_callback itself.
//
//export box_set_log_callback
func box_set_log_callback(fn C.sovereign_log_fn, ctx unsafe.Pointer) {
	setLogCallback(fn, ctx)
}

// box_free releases a string previously returned by this DLL. Go's
// C.CString allocates with this DLL's own C allocator (MinGW/UCRT); the
// C++ host runs a different CRT instance (MSVC), so it must not call its
// own free() on that pointer — cross-CRT-heap free is undefined
// behavior. Every exported function that returns a string gets a
// matching _free counterpart; callers must use it.
//
//export box_free
func box_free(ptr *C.char) {
	C.free(unsafe.Pointer(ptr))
}

func main() {}
