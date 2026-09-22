// Package main builds as a c-shared DLL (sovereign-gocore.dll) hosting
// sing-box inside the service process. P2 atom 1a: prove the DLL builds
// with sing-box actually linked in and that a C++ host can load it and
// call an export without crashing — box_start/box_stop/box_stats and
// real sing-box calls are 1b/1c, not here.
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"fmt"
	"unsafe"

	"github.com/sagernet/sing-box/constant"
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
