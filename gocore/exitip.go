package main

// The exit: the address the internet sees through an outbound, its country
// and its network's owner (the ISP, by the address's AS), for the tray's bar
// and its list of servers. Asked through the outbound itself (like
// sing-box's URL test dials), so it's the proxy's exit whatever the route
// rules would do with the tray's own traffic. A lookup takes a second or
// more and the service answers its pipe clients one at a time, so
// box_exitip only starts one and reports what it knows - per outbound, a
// few at a time.

/*
#include <stdlib.h>
*/
import "C"

import (
	"bufio"
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"regexp"
	"strings"
	"sync"
	"time"
	"unicode"
	"unicode/utf8"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/constant"
	M "github.com/sagernet/sing/common/metadata"
	"github.com/sagernet/sing/common/ntp"
)

const (
	exitIPTimeout  = 15 * time.Second // a lookup, all its sources together
	exitIPMaxBody  = 8192
	exitIPParallel = 4   // lookups at a time: a list of servers asks for each
	exitIPMaxTags  = 512 // outbounds with a state kept, per box
	exitISPMaxRune = 64
)

// Where an exit is asked, in order: the first that answers with an address
// wins. ipinfo places hosting addresses where they are (checked 02.10.2026:
// servers sold as NL, PL, DE, EE - ip.sb said AE, AE, BG, AE, their
// registration); ip.sb and ipinfo say the network's owner too, Cloudflare
// only the country.
var exitIPSources = []struct {
	url   string
	parse func([]byte) exitIPState
}{
	{"https://ipinfo.io/json", parseIPInfo},
	{"https://api.ip.sb/geoip", parseIPSB},
	{"https://www.cloudflare.com/cdn-cgi/trace", parseTrace},
}

type exitIPState struct {
	Tag     string `json:"tag"`
	IP      string `json:"ip,omitempty"`
	Country string `json:"country,omitempty"` // ISO 3166-1 alpha-2
	ISP     string `json:"isp,omitempty"`     // the AS's organization: "Hetzner Online GmbH"
	Error   string `json:"error,omitempty"`
	Pending bool   `json:"pending,omitempty"`
}

type exitIPEntry struct {
	state exitIPState
	round int64 // which lookup may write it: a newer one (or a box restart) drops an older one's result
}

// exitMu guards the states; exitSlots holds the lookups running.
var (
	exitMu     sync.Mutex
	exitStates = map[string]*exitIPEntry{}
	exitRound  int64
	exitSlots  = make(chan struct{}, exitIPParallel)
)

func resetExitIP() {
	exitMu.Lock()
	defer exitMu.Unlock()
	exitStates = map[string]*exitIPEntry{}
	exitRound++
}

type exitIPRequest struct {
	Tag     string `json:"tag"`
	Refresh bool   `json:"refresh"`
}

// box_exitip reports the exit through the outbound `tag` as JSON
// ({"tag", "ip", "country", "isp", "error", "pending"}), and starts a lookup
// when there's none for that tag yet or `refresh` asks for a new one (not
// while one runs). requestJSON: {"tag": "...", "refresh": false} (validated
// by the service). Freed with box_free.
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
	entry, known := exitStates[request.Tag]
	if known && (!request.Refresh || entry.state.Pending) {
		return marshalExitIP(entry.state)
	}
	outbound, found := instance.Outbound().Outbound(request.Tag)
	if !found {
		return marshalExitIP(exitIPState{Tag: request.Tag, Error: "no such outbound"})
	}
	if !known {
		if len(exitStates) >= exitIPMaxTags {
			return marshalExitIP(exitIPState{Tag: request.Tag, Error: "too many outbounds asked"})
		}
		entry = &exitIPEntry{}
		exitStates[request.Tag] = entry
	}
	exitRound++
	round := exitRound
	entry.round = round
	entry.state = exitIPState{Tag: request.Tag, Pending: true}
	go func() {
		exitSlots <- struct{}{}
		result := lookupExitIP(ctx, outbound)
		<-exitSlots
		result.Tag = request.Tag
		exitMu.Lock()
		defer exitMu.Unlock()
		if current, ok := exitStates[request.Tag]; ok && current.round == round {
			current.state = result
		}
	}()
	return marshalExitIP(entry.state)
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
	// The first answer with an address gives the exit; one without the
	// network's owner (ipinfo has none for some IPv6) takes it from the next
	// source that names it for the same address.
	var found exitIPState
	var failure string
	for _, source := range exitIPSources {
		body, err := fetchExitIP(ctx, &client, source.url)
		if err == nil {
			state := source.parse(body)
			switch {
			case state.IP == "":
				err = fmt.Errorf("no ip in the answer")
			case found.IP == "":
				found = state
			case state.IP == found.IP && found.ISP == "":
				found.ISP = state.ISP
			}
			if found.IP != "" && found.ISP != "" {
				return found
			}
		}
		if err != nil && failure == "" {
			failure = err.Error()
		}
		if ctx.Err() != nil {
			break
		}
	}
	if found.IP != "" {
		return found
	}
	return exitIPState{Error: failure}
}

func fetchExitIP(ctx context.Context, client *http.Client, url string) ([]byte, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	// What the tray sends everywhere: the core, nothing of Sovereign's.
	request.Header.Set("User-Agent", "sing-box "+constant.Version)
	response, err := client.Do(request)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("HTTP %d", response.StatusCode)
	}
	return io.ReadAll(io.LimitReader(response.Body, exitIPMaxBody))
}

func exitAddress(value string) string {
	if ip := net.ParseIP(strings.TrimSpace(value)); ip != nil {
		return ip.String()
	}
	return ""
}

func exitCountry(value string) string {
	value = strings.ToUpper(strings.TrimSpace(value))
	if len(value) == 2 && value[0] >= 'A' && value[0] <= 'Z' && value[1] >= 'A' && value[1] <= 'Z' {
		return value
	}
	return ""
}

// exitISP: one line of printable text, at most exitISPMaxRune characters.
func exitISP(value string) string {
	if !utf8.ValidString(value) {
		return ""
	}
	value = strings.Join(strings.FieldsFunc(value, func(r rune) bool {
		return unicode.IsSpace(r) || !unicode.IsPrint(r)
	}), " ")
	if utf8.RuneCountInString(value) > exitISPMaxRune {
		value = string([]rune(value)[:exitISPMaxRune])
	}
	return value
}

// parseIPSB reads api.ip.sb/geoip: ip, country_code, asn_organization (or isp).
func parseIPSB(body []byte) exitIPState {
	var answer struct {
		IP           string `json:"ip"`
		CountryCode  string `json:"country_code"`
		Organization string `json:"asn_organization"`
		ISP          string `json:"isp"`
	}
	if json.Unmarshal(body, &answer) != nil {
		return exitIPState{}
	}
	isp := answer.Organization
	if strings.TrimSpace(isp) == "" {
		isp = answer.ISP
	}
	return exitIPState{IP: exitAddress(answer.IP), Country: exitCountry(answer.CountryCode), ISP: exitISP(isp)}
}

var asPrefix = regexp.MustCompile(`^AS\d+\s+`)

// parseIPInfo reads ipinfo.io/json: ip, country, org ("AS24940 Hetzner Online GmbH").
func parseIPInfo(body []byte) exitIPState {
	var answer struct {
		IP      string `json:"ip"`
		Country string `json:"country"`
		Org     string `json:"org"`
	}
	if json.Unmarshal(body, &answer) != nil {
		return exitIPState{}
	}
	return exitIPState{IP: exitAddress(answer.IP), Country: exitCountry(answer.Country),
		ISP: exitISP(asPrefix.ReplaceAllString(strings.TrimSpace(answer.Org), ""))}
}

// parseTrace reads Cloudflare's "key=value" lines: ip= and loc=.
func parseTrace(body []byte) exitIPState {
	var state exitIPState
	scanner := bufio.NewScanner(bytes.NewReader(body))
	for scanner.Scan() {
		key, value, found := strings.Cut(scanner.Text(), "=")
		if !found {
			continue
		}
		switch key {
		case "ip":
			state.IP = exitAddress(value)
		case "loc":
			state.Country = exitCountry(value)
		}
	}
	return state
}
