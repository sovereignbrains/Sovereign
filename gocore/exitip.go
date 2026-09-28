package main

// The exit IP: the address the internet sees through an outbound, and its
// country, for the tray's bar. Asked of Cloudflare's trace endpoint through
// the outbound itself (like sing-box's URL test dials), so it's the proxy's
// exit whatever the route rules would do with the tray's own traffic. A
// lookup takes a second or more and the service answers its pipe clients one
// at a time, so box_exitip only starts one and reports what it knows.

/*
#include <stdlib.h>
*/
import "C"

import (
	"bufio"
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing/common/ntp"
	M "github.com/sagernet/sing/common/metadata"
)

const (
	exitIPURL     = "https://www.cloudflare.com/cdn-cgi/trace"
	exitIPTimeout = 10 * time.Second
	exitIPMaxBody = 4096
)

type exitIPState struct {
	Tag     string `json:"tag"`
	IP      string `json:"ip,omitempty"`
	Country string `json:"country,omitempty"` // ISO 3166-1 alpha-2, as Cloudflare says
	Error   string `json:"error,omitempty"`
	Pending bool   `json:"pending,omitempty"`
}

// exitMu guards the one lookup's state; exitRound drops a result that a
// newer lookup (or a box restart) overtook.
var (
	exitMu    sync.Mutex
	exitState exitIPState
	exitRound int64
)

func resetExitIP() {
	exitMu.Lock()
	defer exitMu.Unlock()
	exitState = exitIPState{}
	exitRound++
}

type exitIPRequest struct {
	Tag     string `json:"tag"`
	Refresh bool   `json:"refresh"`
}

// box_exitip reports the exit IP through the outbound `tag` as JSON
// ({"tag", "ip", "country", "error", "pending"}), and starts a lookup when
// there's none for that tag yet or `refresh` asks for a new one.
// requestJSON: {"tag": "...", "refresh": false} (validated by the service).
// Freed with box_free.
//
//export box_exitip
func box_exitip(requestJSON *C.char) *C.char {
	var request exitIPRequest
	if err := json.Unmarshal([]byte(C.GoString(requestJSON)), &request); err != nil {
		return marshalExitIP(exitIPState{Error: fmt.Sprintf("parse request: %s", err)})
	}
	boxMu.Lock()
	instance, ctx := boxInstance, boxContext
	boxMu.Unlock()
	if instance == nil {
		return marshalExitIP(exitIPState{Tag: request.Tag, Error: "box not running"})
	}

	exitMu.Lock()
	defer exitMu.Unlock()
	if exitState.Tag == request.Tag && !request.Refresh {
		return marshalExitIP(exitState)
	}
	exitRound++
	round := exitRound
	exitState = exitIPState{Tag: request.Tag, Pending: true}
	outbound, found := instance.Outbound().Outbound(request.Tag)
	if !found {
		exitState = exitIPState{Tag: request.Tag, Error: "no such outbound"}
		return marshalExitIP(exitState)
	}
	go func() {
		result := lookupExitIP(ctx, outbound)
		result.Tag = request.Tag
		exitMu.Lock()
		defer exitMu.Unlock()
		if exitRound == round {
			exitState = result
		}
	}()
	return marshalExitIP(exitState)
}

func marshalExitIP(state exitIPState) *C.char {
	out, err := json.Marshal(state)
	if err != nil {
		return C.CString(`{"error":"marshal"}`)
	}
	return C.CString(string(out))
}

func lookupExitIP(ctx context.Context, outbound adapter.Outbound) exitIPState {
	ctx, cancel := context.WithTimeout(ctx, exitIPTimeout)
	defer cancel()
	client := http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
				return outbound.DialContext(ctx, "tcp", M.ParseSocksaddr(addr))
			},
			TLSClientConfig: &tls.Config{
				Time:    ntp.TimeFuncFromContext(ctx),
				RootCAs: adapter.RootPoolFromContext(ctx),
			},
			DisableKeepAlives: true,
		},
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, exitIPURL, nil)
	if err != nil {
		return exitIPState{Error: err.Error()}
	}
	response, err := client.Do(request)
	if err != nil {
		return exitIPState{Error: err.Error()}
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return exitIPState{Error: fmt.Sprintf("HTTP %d", response.StatusCode)}
	}
	return parseTrace(io.LimitReader(response.Body, exitIPMaxBody))
}

// parseTrace reads Cloudflare's "key=value" lines: ip= and loc=.
func parseTrace(body io.Reader) exitIPState {
	var state exitIPState
	scanner := bufio.NewScanner(body)
	for scanner.Scan() {
		key, value, found := strings.Cut(scanner.Text(), "=")
		if !found {
			continue
		}
		switch key {
		case "ip":
			if ip := net.ParseIP(value); ip != nil {
				state.IP = ip.String()
			}
		case "loc":
			if len(value) == 2 && value[0] >= 'A' && value[0] <= 'Z' && value[1] >= 'A' && value[1] <= 'Z' {
				state.Country = value
			}
		}
	}
	if state.IP == "" {
		state.Error = "no ip in the answer"
	}
	return state
}
