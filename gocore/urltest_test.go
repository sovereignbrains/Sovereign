package main

import (
	"context"
	"os"
	"sort"
	"testing"
	"time"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/option"
	singjson "github.com/sagernet/sing/common/json"
)

func ms(values ...int) []time.Duration {
	out := make([]time.Duration, 0, len(values))
	for _, v := range values {
		out = append(out, time.Duration(v)*time.Millisecond)
	}
	return out
}

func TestQuartilesAndSettled(t *testing.T) {
	q1, q2, q3 := quartiles(ms(100, 90, 110, 95, 105))
	if q2 != 100*time.Millisecond || q1 != 95*time.Millisecond || q3 != 105*time.Millisecond {
		t.Fatalf("quartiles: %v %v %v", q1, q2, q3)
	}
	if !settled(ms(100, 102, 98, 101, 99)) {
		t.Fatal("a tight series must count as settled")
	}
	if settled(ms(100, 400, 90, 800, 120)) {
		t.Fatal("a scattered series must not count as settled")
	}
	if !settled(ms(5, 12, 7, 9, 6)) {
		t.Fatal("within 10 ms counts as settled even when it's a large share")
	}
	if median(ms(300)) != 300*time.Millisecond {
		t.Fatal("median of one")
	}
}

// SOVEREIGN_PROBE_CONFIG=<a sing-box config with no TUN> go test -run
// TestProbeLive -v: every outbound of it measured, as the tray would.
func TestProbeLive(t *testing.T) {
	path := os.Getenv("SOVEREIGN_PROBE_CONFIG")
	if path == "" {
		t.Skip("SOVEREIGN_PROBE_CONFIG not set")
	}
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	ctx := include.Context(context.Background())
	var options option.Options
	if err := singjson.UnmarshalContext(ctx, content, &options); err != nil {
		t.Fatal(err)
	}
	instance, err := box.New(box.Options{Context: ctx, Options: options})
	if err != nil {
		t.Fatal(err)
	}
	if err := instance.Start(); err != nil {
		t.Fatal(err)
	}
	defer instance.Close()
	var tags []string
	for _, outbound := range instance.Outbound().Outbounds() {
		switch outbound.Type() {
		case "direct", "block", "dns", "selector", "urltest":
			continue
		}
		tags = append(tags, outbound.Tag())
	}
	sort.Strings(tags)
	for round := 1; round <= 2; round++ {
		for _, tag := range tags {
			outbound, _ := instance.Outbound().Outbound(tag)
			started := time.Now()
			r := probe(ctx, "", outbound, 5*time.Second)
			t.Logf("round %d %-28s delay %4d ms ±%3d  loss %3d%%  samples %2d  connect %4d ms  (%.1fs) %s",
				round, tag, r.Delay, r.Jitter, r.Loss, r.Samples, r.Connect, time.Since(started).Seconds(), r.Error)
		}
	}
}
