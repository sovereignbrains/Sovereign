package main

// Latency tests of the running box's outbounds, for the tray's server list
// and its "auto" choice. sing-box's own URL test (common/urltest) times one
// request over a fresh connection - the dial through the server, TLS to the
// test site and the request in one number, once: run twice it gives 360 and
// 800 ms for the same server, and one lost packet reads as "no answer".
//
// Here each outbound gets one connection - how long that took is reported
// apart, as "connect" - and then requests over it, one after another, each
// a single round trip through the server: as many as it takes for the
// numbers to settle (at least minSamples, at most maxSamples, within
// probeBudget), reported as the median, the spread (half the interquartile
// range) and the share lost. A request that fails is counted lost and the
// connection is made again.
//
// A test takes seconds and the service answers its pipe clients one at a
// time, so box_urltest only starts one and returns; box_delays reports how
// far it got.

/*
#include <stdlib.h>
*/
import "C"

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"sort"
	"sync"
	"time"

	"github.com/sagernet/sing-box/adapter"
	M "github.com/sagernet/sing/common/metadata"
	N "github.com/sagernet/sing/common/network"
	"github.com/sagernet/sing/common/ntp"
)

// At most this many outbounds are tested at once: a subscription with dozens
// of servers must not open dozens of handshakes in the same instant.
const urlTestParallel = 8

const (
	minSamples    = 5
	maxSamples    = 15
	probeBudget   = 12 * time.Second // one outbound's test, all of it
	sampleSpacing = 120 * time.Millisecond
)

type delayResult struct {
	Tag     string `json:"tag"`
	Delay   uint16 `json:"delay,omitempty"`   // ms: the median round trip through the server
	Jitter  uint16 `json:"jitter,omitempty"`  // ms: half the interquartile range
	Loss    uint8  `json:"loss,omitempty"`    // percent of the requests lost
	Samples uint8  `json:"samples,omitempty"` // requests that came back
	Connect uint16 `json:"connect,omitempty"` // ms: a connection through the server, TLS and the first request
	Error   string `json:"error,omitempty"`   // set when none came back
	Pending bool   `json:"pending,omitempty"` // still running
}

// delayMu guards the results. They belong to one box_start (resetDelays) and
// are keyed by tag; a newer test of a tag overwrites the older result, and a
// result from a test started before the last reset or overtaken by a newer
// test of the same tag is dropped (delayRound).
var (
	delayMu     sync.Mutex
	delayOrder  []string
	delays      = map[string]*delayResult{}
	delayRounds = map[string]int64{}
	delayRound  int64
)

func resetDelays() {
	delayMu.Lock()
	defer delayMu.Unlock()
	delayOrder = nil
	delays = map[string]*delayResult{}
	delayRounds = map[string]int64{}
	delayRound++
}

type urlTestRequest struct {
	Tags      []string `json:"tags"`
	URL       string   `json:"url"`
	TimeoutMs int      `json:"timeout_ms"`
}

// box_urltest starts a latency test of the running box's outbounds.
// requestJSON: {"tags": [...], "url": "https://...", "timeout_ms": 5000}
// (the service has validated it; timeout_ms bounds each request). Returns ""
// once the test is under way, or why it could not start; the string is
// freed with box_free.
//
//export box_urltest
func box_urltest(requestJSON *C.char) *C.char {
	var request urlTestRequest
	if err := json.Unmarshal([]byte(C.GoString(requestJSON)), &request); err != nil {
		return C.CString(fmt.Sprintf("parse request: %s", err))
	}
	boxMu.Lock()
	instance, ctx := boxInstance, boxContext
	boxMu.Unlock()
	if instance == nil {
		return C.CString("box not running")
	}

	delayMu.Lock()
	delayRound++
	round := delayRound
	for _, tag := range request.Tags {
		if _, seen := delays[tag]; !seen {
			delayOrder = append(delayOrder, tag)
		}
		delays[tag] = &delayResult{Tag: tag, Pending: true}
		delayRounds[tag] = round
	}
	delayMu.Unlock()

	timeout := time.Duration(request.TimeoutMs) * time.Millisecond
	outbounds := instance.Outbound()
	go func() {
		slots := make(chan struct{}, urlTestParallel)
		var wg sync.WaitGroup
		for _, tag := range request.Tags {
			wg.Add(1)
			slots <- struct{}{}
			go func(tag string) {
				defer wg.Done()
				defer func() { <-slots }()
				var result delayResult
				if outbound, found := outbounds.Outbound(tag); !found {
					result = delayResult{Tag: tag, Error: "no such outbound"}
				} else {
					result = probe(ctx, request.URL, outbound, timeout)
					result.Tag = tag
				}
				delayMu.Lock()
				defer delayMu.Unlock()
				if delayRounds[tag] == round {
					delays[tag] = &result
				}
			}(tag)
		}
		wg.Wait()
	}()
	return C.CString("")
}

// One connection's worth of requests: an HTTP client over a single
// connection through the outbound, kept alive between requests.
type probeClient struct {
	client *http.Client
}

func newProbeClient(ctx context.Context, dialer N.Dialer, destination M.Socksaddr, timeout time.Duration) *probeClient {
	var used bool
	var mu sync.Mutex
	transport := &http.Transport{
		DialContext: func(dialCtx context.Context, _, _ string) (net.Conn, error) {
			mu.Lock()
			defer mu.Unlock()
			if used {
				return nil, errors.New("connection lost")
			}
			used = true
			return dialer.DialContext(dialCtx, N.NetworkTCP, destination)
		},
		TLSClientConfig: &tls.Config{
			Time:    ntp.TimeFuncFromContext(ctx),
			RootCAs: adapter.RootPoolFromContext(ctx),
		},
		MaxIdleConnsPerHost: 1,
		IdleConnTimeout:     30 * time.Second,
	}
	return &probeClient{client: &http.Client{
		Transport: transport,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		},
		Timeout: timeout,
	}}
}

func (p *probeClient) head(ctx context.Context, link string) (time.Duration, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodHead, link, nil)
	if err != nil {
		return 0, err
	}
	start := time.Now()
	response, err := p.client.Do(request)
	if err != nil {
		return 0, err
	}
	_, _ = io.Copy(io.Discard, response.Body)
	response.Body.Close()
	return time.Since(start), nil
}

func (p *probeClient) close() { p.client.CloseIdleConnections() }

// probe measures one outbound as described at the top.
func probe(ctx context.Context, link string, outbound adapter.Outbound, timeout time.Duration) delayResult {
	if link == "" {
		link = "https://www.gstatic.com/generate_204"
	}
	if timeout <= 0 || timeout > 5*time.Second {
		timeout = 5 * time.Second
	}
	parsed, err := url.Parse(link)
	if err != nil {
		return delayResult{Error: err.Error()}
	}
	port := parsed.Port()
	if port == "" {
		port = "443"
		if parsed.Scheme == "http" {
			port = "80"
		}
	}
	destination := M.ParseSocksaddrHostPortStr(parsed.Hostname(), port)

	probeCtx, cancel := context.WithTimeout(ctx, probeBudget)
	defer cancel()
	var rtts, connects []time.Duration
	lost := 0
	var lastErr error
	var client *probeClient
	for len(rtts)+lost < maxSamples && probeCtx.Err() == nil {
		if client == nil {
			// A new connection: its first request carries the dial through
			// the server and the TLS handshake - "connect", not a round trip.
			client = newProbeClient(ctx, outbound, destination, timeout)
			took, err := client.head(probeCtx, link)
			if err != nil {
				lost++
				lastErr = err
				client.close()
				client = nil
				if len(rtts) == 0 && lost >= 3 {
					break // nothing comes back at all
				}
				continue
			}
			connects = append(connects, took)
			continue
		}
		took, err := client.head(probeCtx, link)
		if err != nil {
			lost++
			lastErr = err
			client.close()
			client = nil
			continue
		}
		rtts = append(rtts, took)
		if len(rtts) >= minSamples && settled(rtts) {
			break
		}
		select {
		case <-probeCtx.Done():
		case <-time.After(sampleSpacing):
		}
	}
	if client != nil {
		client.close()
	}

	result := delayResult{}
	if len(connects) > 0 {
		result.Connect = millis(median(connects))
	}
	if len(rtts) == 0 {
		if lastErr == nil {
			lastErr = errors.New("no answer")
		}
		result.Error = lastErr.Error()
		return result
	}
	q1, q2, q3 := quartiles(rtts)
	result.Delay = max(millis(q2), 1)
	result.Jitter = millis((q3 - q1) / 2)
	result.Samples = uint8(len(rtts))
	result.Loss = uint8(lost * 100 / (len(rtts) + lost))
	return result
}

// settled: the middle half of the round trips lies within 15% of the median
// (or within 10 ms, for the fast ones).
func settled(samples []time.Duration) bool {
	q1, q2, q3 := quartiles(samples)
	spread := q3 - q1
	return spread <= 10*time.Millisecond || float64(spread) <= 0.15*float64(q2)
}

func quartiles(samples []time.Duration) (time.Duration, time.Duration, time.Duration) {
	sorted := append([]time.Duration(nil), samples...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
	at := func(q float64) time.Duration {
		position := q * float64(len(sorted)-1)
		low := int(position)
		high := min(low+1, len(sorted)-1)
		fraction := position - float64(low)
		return sorted[low] + time.Duration(fraction*float64(sorted[high]-sorted[low]))
	}
	return at(0.25), at(0.5), at(0.75)
}

func median(samples []time.Duration) time.Duration {
	_, q2, _ := quartiles(samples)
	return q2
}

func millis(d time.Duration) uint16 {
	ms := d / time.Millisecond
	if ms > 65535 {
		return 65535
	}
	return uint16(ms)
}

// box_delays returns the results so far as JSON: {"results": [{"tag", and
// "delay", "jitter", "loss", "samples", "connect" - or "error", or "pending":
// true}]}, in the order tags were first tested. Freed with box_free.
//
//export box_delays
func box_delays() *C.char {
	delayMu.Lock()
	results := make([]delayResult, 0, len(delayOrder))
	for _, tag := range delayOrder {
		results = append(results, *delays[tag])
	}
	delayMu.Unlock()
	out, err := json.Marshal(struct {
		Results []delayResult `json:"results"`
	}{results})
	if err != nil {
		return C.CString(`{"results":[]}`)
	}
	return C.CString(string(out))
}

// box_select switches a selector outbound to one of its options, now, for
// new connections (sing-box keeps the choice in its cache file, and a
// selector starts on its cached choice, not its "default"). Returns "" or why
// not; freed with box_free.
//
//export box_select
func box_select(selectorTag *C.char, outboundTag *C.char) *C.char {
	boxMu.Lock()
	instance := boxInstance
	boxMu.Unlock()
	if instance == nil {
		return C.CString("box not running")
	}
	outbound, found := instance.Outbound().Outbound(C.GoString(selectorTag))
	if !found {
		return C.CString("no such selector")
	}
	selector, ok := outbound.(interface{ SelectOutbound(tag string) bool })
	if !ok {
		return C.CString("not a selector")
	}
	if !selector.SelectOutbound(C.GoString(outboundTag)) {
		return C.CString("no such option in the selector")
	}
	return C.CString("")
}
