package main

import (
	"context"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/option"
	singjson "github.com/sagernet/sing/common/json"
)

// The exit lookups' answers, as the services give them (trimmed), and what
// a broken or hostile one leaves.
func TestExitIPParsers(t *testing.T) {
	cases := []struct {
		name  string
		parse func([]byte) exitIPState
		body  string
		want  exitIPState
	}{
		{"ip.sb", parseIPSB,
			`{"organization":"Brainoza OU","isp":"Brainoza OU","asn_organization":"Brainoza OU","asn":214790,"ip":"5.181.201.59","country":"Estonia","country_code":"EE"}`,
			exitIPState{IP: "5.181.201.59", Country: "EE", ISP: "Brainoza OU"}},
		{"ip.sb without the AS's organization", parseIPSB,
			`{"isp":"Hetzner Online GmbH","ip":"2a01:04f8:0000:0000:0000:0000:0000:0001","country_code":"de"}`,
			exitIPState{IP: "2a01:4f8::1", Country: "DE", ISP: "Hetzner Online GmbH"}},
		{"ipinfo", parseIPInfo,
			`{"ip":"5.181.201.59","city":"Tallinn","country":"EE","org":"AS214790 Brainoza OU"}`,
			exitIPState{IP: "5.181.201.59", Country: "EE", ISP: "Brainoza OU"}},
		{"cloudflare", parseTrace, "fl=1\nip=185.12.34.56\nloc=NL\nwarp=off\n",
			exitIPState{IP: "185.12.34.56", Country: "NL"}},
		{"cloudflare over Tor", parseTrace, "ip=185.12.34.56\nloc=T1\n", exitIPState{IP: "185.12.34.56"}},
		{"not json", parseIPSB, "<html>blocked</html>", exitIPState{}},
		{"wrong fields", parseIPInfo, `{"ip":"not an address","country":"Estonia","org":5}`, exitIPState{}},
		{"control characters and a long name", parseIPSB,
			`{"ip":"1.2.3.4","country_code":"NL","asn_organization":"Evil\r\nSet-Cookie: x\u0000 ` +
				`AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}`,
			exitIPState{IP: "1.2.3.4", Country: "NL",
				ISP: "Evil Set-Cookie: x " + strings.Repeat("A", exitISPMaxRune-19)}},
	}
	for _, c := range cases {
		if got := c.parse([]byte(c.body)); got != c.want {
			t.Errorf("%s: got %+v, want %+v", c.name, got, c.want)
		}
	}
	if got := exitISP("Аэза Интернэшнл"); got != "Аэза Интернэшнл" {
		t.Errorf("non-Latin ISP: got %q", got)
	}
}

// Every server of a real config looked up as the tray does, a few at a time
// (SOVEREIGN_PROBE_CONFIG: a config without inbounds, as for TestProbeLive).
func TestExitIPLive(t *testing.T) {
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
	var wait sync.WaitGroup
	for _, outbound := range instance.Outbound().Outbounds() {
		switch outbound.Type() {
		case "direct", "block", "dns", "selector", "urltest":
			continue
		}
		wait.Add(1)
		go func() {
			defer wait.Done()
			exitSlots <- struct{}{}
			started := time.Now()
			state := lookupExitIP(ctx, outbound)
			<-exitSlots
			t.Logf("%-32s %-15s %-2s %-28q (%.1fs) %s", outbound.Tag(), state.IP, state.Country, state.ISP,
				time.Since(started).Seconds(), state.Error)
		}()
	}
	wait.Wait()
}
