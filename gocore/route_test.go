package main

import (
	"net/netip"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/common/trafficcontrol"
	M "github.com/sagernet/sing/common/metadata"
)

func connection(at int, domain, address, outbound string, chain ...string) *trafficcontrol.TrackerMetadata {
	m := &trafficcontrol.TrackerMetadata{
		CreatedAt: time.Unix(int64(at), 0),
		Outbound:  outbound,
		Chain:     chain,
		Metadata:  adapter.InboundContext{Domain: domain},
	}
	if address != "" {
		m.Metadata.Destination = M.SocksaddrFrom(netip.MustParseAddr(address), 443)
	}
	return m
}

func TestRouteOfTheNewestByNameOrAddress(t *testing.T) {
	open := []*trafficcontrol.TrackerMetadata{
		connection(3, "", "77.88.55.242", "direct", "direct"), // ya.ru by its address, sniffing aside
		connection(1, "chatgpt.com", "104.18.32.47", "AnyTLS", "AnyTLS", "proxy"),
	}
	closed := []*trafficcontrol.TrackerMetadata{
		connection(2, "ya.ru", "77.88.55.242", "AnyTLS", "AnyTLS", "proxy"), // older: before a rule changed
		nil,
	}
	if got := RouteOf([]string{"ya.ru", "77.88.55.242"}, open, closed); got == nil || got.Outbound != "direct" {
		t.Fatalf("ya.ru = %+v", got)
	}
	if got := RouteOf([]string{" ChatGPT.com. "}, open, closed); got == nil || got.Outbound != "AnyTLS" ||
		len(got.Chain) != 2 || got.Chain[1] != "proxy" || got.Rule != "" {
		t.Fatalf("chatgpt.com = %+v", got)
	}
	if got := RouteOf([]string{"example.com", ""}, open, closed); got != nil {
		t.Fatalf("example.com = %+v", got)
	}
	if got := RouteOf(nil); got != nil {
		t.Fatalf("nothing = %+v", got)
	}
}

func TestShortenedOnACharactersBoundary(t *testing.T) {
	if got := shortened("ya.ru", 400); got != "ya.ru" {
		t.Fatalf("short = %q", got)
	}
	got := shortened("домены=ya.ru vk.com", 10) // 2 bytes a letter
	if len(got) > 10 || !utf8.ValidString(got) || got != "дом…" {
		t.Fatalf("cut = %q (%d bytes)", got, len(got))
	}
}
