package main

// Latency tests of the running box's outbounds, for the tray's server list:
// sing-box's own URL test (common/urltest, what its urltest groups use),
// one HEAD request through each outbound. A test takes seconds and the
// service answers its pipe clients one at a time, so box_urltest only
// starts one and returns; box_delays reports how far it got.

/*
#include <stdlib.h>
*/
import "C"

import (
	"context"
	"encoding/json"
	"fmt"
	"sync"
	"time"

	"github.com/sagernet/sing-box/common/urltest"
)

// At most this many outbounds are dialed at once: a subscription with dozens
// of servers must not open dozens of handshakes in the same instant.
const urlTestParallel = 8

type delayResult struct {
	Tag     string `json:"tag"`
	Delay   uint16 `json:"delay,omitempty"`   // milliseconds; set when the test passed
	Error   string `json:"error,omitempty"`   // set when it failed
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
// (the service has validated it). Returns "" once the test is under way, or
// why it could not start; the string is freed with box_free.
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
				result := delayResult{Tag: tag}
				if outbound, found := outbounds.Outbound(tag); !found {
					result.Error = "no such outbound"
				} else {
					testCtx, cancel := context.WithTimeout(ctx, timeout)
					delay, err := urltest.URLTest(testCtx, request.URL, outbound)
					cancel()
					if err != nil {
						result.Error = err.Error()
					} else {
						result.Delay = max(delay, 1)
					}
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

// box_delays returns the results so far as JSON: {"results": [{"tag", and
// one of "delay" (ms), "error", "pending": true}]}, in the order tags were
// first tested. Freed with box_free.
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
