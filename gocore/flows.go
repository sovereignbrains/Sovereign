package main

// Where the traffic went: bytes up and down per outbound a rule sent it to -
// "direct", the WARP endpoint, a proxy selector - for the main window's live
// streams round the switch (each way drawn its own). The tray polls it and
// takes rates from the differences, as with box_stats.

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"

	"github.com/sagernet/sing-box/common/trafficcontrol"
)

// FlowTotals sums connections by the outbound their rule picked: the last of
// the chain (sing-box keeps it from the server back to the rule's outbound).
// A connection with no chain counts under "".
func FlowTotals(connections ...[]*trafficcontrol.TrackerMetadata) map[string][2]int64 {
	flows := map[string][2]int64{}
	for _, list := range connections {
		for _, m := range list {
			tag := ""
			if n := len(m.Chain); n > 0 {
				tag = m.Chain[n-1]
			}
			f := flows[tag]
			if m.Upload != nil {
				f[0] += m.Upload.Load()
			}
			if m.Download != nil {
				f[1] += m.Download.Load()
			}
			flows[tag] = f
		}
	}
	return flows
}

// box_flows reports the running instance's traffic by outbound: open
// connections plus the closed ones the manager still keeps (the oldest fall
// off its list - a total may drop; the tray counts a drop as nothing).
// {"flows":{"<tag>":[up,down],...}}, empty while nothing runs. Freed with
// box_free.
//
//export box_flows
func box_flows() *C.char {
	boxMu.Lock()
	defer boxMu.Unlock()
	flows := map[string][2]int64{}
	if boxTraffic != nil {
		flows = FlowTotals(boxTraffic.Connections(), boxTraffic.ClosedConnections())
	}
	out, err := json.Marshal(map[string]any{"flows": flows})
	if err != nil {
		return C.CString(`{"flows":{}}`)
	}
	return C.CString(string(out))
}
