// Package main builds as a c-shared DLL (sovereign-gocore.dll) hosting
// sing-box inside the service process. P2 atom 1a proved the DLL loads and
// runs Go inside the C++ host (box_ping). P2 atom 1b adds box_start/box_stop:
// a real sing-box instance built from a caller-supplied JSON config, parsed
// through sing-box's own context-aware JSON machinery (include.Context wires
// up the same inbound/outbound registries the real `sing-box run` CLI uses,
// so this is not a reimplementation of config parsing — it's the real thing).
// box_stats and TUN wiring are still 1c, not here.
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"context"
	"fmt"
	"sync"
	"unsafe"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/option"
	singjson "github.com/sagernet/sing/common/json"
)

// boxMu guards boxInstance: cgo exports are just functions, there is no
// per-call object to hang state off, so the running *box.Box (if any) lives
// in a package-level variable. Only one instance runs at a time — matches
// ICore's contract (the service swaps GoCore/NativeCore, not multiple cores
// at once).
var (
	boxMu       sync.Mutex
	boxInstance *box.Box
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
	if err := instance.Start(); err != nil {
		return C.CString(fmt.Sprintf("start box: %s", err))
	}

	boxInstance = instance
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
	boxInstance = nil
	if err != nil {
		return C.CString(fmt.Sprintf("stop box: %s", err))
	}
	return C.CString("")
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
