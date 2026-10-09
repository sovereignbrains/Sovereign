package main

import (
	"testing"
	"time"
)

// A name asked, then the ads list matching that request: said once, again
// only after blockedRepeat; another rule, or another request, says nothing.
func TestBlockedNotice(t *testing.T) {
	w := newBlockedWatch()
	at := time.Unix(1_800_000_000, 0)
	feed := func(line string) string { return w.notice(line, at) }

	if got := feed("+0300 2026-10-09 12:00:00 DEBUG [3712645 0ms] dns: exchange Ads.Example.com. IN A\n"); got != "" {
		t.Fatalf("an ask said %q", got)
	}
	if got := feed("+0300 2026-10-09 12:00:00 DEBUG [3712645 1ms] dns: match[9] rule_set=[sov-ads sov-ads-adguard] => predefined(rcode=NOERROR)\n"); got != "ads: blocked ads.example.com" {
		t.Fatalf("blocked: %q", got)
	}
	// The same name again, from another request: not yet.
	feed("DEBUG [99 0ms] dns: exchange ads.example.com. IN AAAA")
	if got := feed("DEBUG [99 0ms] dns: match[9] rule_set=sov-ads => predefined(rcode=NOERROR)"); got != "" {
		t.Fatalf("said twice: %q", got)
	}
	at = at.Add(blockedRepeat)
	feed("DEBUG [100 0ms] dns: exchange ads.example.com. IN A")
	if got := feed("DEBUG [100 0ms] dns: match[9] rule_set=sov-ads => predefined(rcode=NOERROR)"); got != "ads: blocked ads.example.com" {
		t.Fatalf("after the pause: %q", got)
	}
	// A connection by its sniffed name, rejected by the route's rule.
	feed("DEBUG [7 0ms] router: sniffed protocol: tls, domain: tracker.example.net, client: chrome")
	if got := feed("DEBUG [7 0ms] router: match[4] rule_set=sov-ads => reject"); got != "ads: blocked tracker.example.net" {
		t.Fatalf("sniffed: %q", got)
	}
	// The user's own block, and a match with no name known: nothing.
	feed("DEBUG [8 0ms] dns: exchange yandex.com. IN A")
	if got := feed("DEBUG [8 0ms] dns: match[2] domain_suffix=yandex.com => predefined(rcode=NOERROR)"); got != "" {
		t.Fatalf("the user's rule: %q", got)
	}
	if got := feed("DEBUG [9 0ms] dns: match[9] rule_set=sov-ads => predefined(rcode=NOERROR)"); got != "" {
		t.Fatalf("no name: %q", got)
	}
}
