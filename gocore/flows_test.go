package main

import (
	"sync/atomic"
	"testing"

	"github.com/sagernet/sing-box/common/trafficcontrol"
)

func tracked(up, down int64, chain ...string) *trafficcontrol.TrackerMetadata {
	u := new(atomic.Int64)
	u.Store(up)
	d := new(atomic.Int64)
	d.Store(down)
	return &trafficcontrol.TrackerMetadata{Upload: u, Download: d, Chain: chain}
}

func TestFlowTotalsByTheRulesOutbound(t *testing.T) {
	open := []*trafficcontrol.TrackerMetadata{
		tracked(10, 100, "Hysteria 2", "auto", "proxy"), // the server, the group, the rule's selector
		tracked(1, 5, "direct"),
	}
	closed := []*trafficcontrol.TrackerMetadata{
		tracked(2, 20, "AnyTLS", "proxy"),
		tracked(3, 30, "warp"),
		{Chain: nil}, // no counters, no chain
	}
	flows := FlowTotals(open, closed)
	if got := flows["proxy"]; got != [2]int64{12, 120} {
		t.Fatalf("proxy = %v", got)
	}
	if got := flows["direct"]; got != [2]int64{1, 5} {
		t.Fatalf("direct = %v", got)
	}
	if got := flows["warp"]; got != [2]int64{3, 30} {
		t.Fatalf("warp = %v", got)
	}
	if got := flows[""]; got != [2]int64{0, 0} {
		t.Fatalf("no chain = %v", got)
	}
}
