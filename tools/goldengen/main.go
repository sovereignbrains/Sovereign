// Command goldengen produces golden JSON fixtures by marshaling real
// sing-box option types through sing-box's own JSON machinery (pinned
// version — see tools/codegen/sing-box.version). The C++ side unmarshals
// this fixture and re-marshals it; the two must be structurally identical.
// This is what "golden" means here: the reference is the real upstream
// marshaler, not our guess at its behavior.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"math"
	"os"
	"time"

	"github.com/sagernet/sing-box/option"
	singjson "github.com/sagernet/sing/common/json"
	"github.com/sagernet/sing/common/json/badoption"
)

func main() {
	fixture := flag.String("fixture", "anytls_outbound", "which fixture to emit")
	out := flag.String("out", "-", "output path ('-' for stdout)")
	flag.Parse()

	var data []byte
	var err error
	switch *fixture {
	case "anytls_outbound":
		data, err = anyTLSOutbound()
	case "anytls_outbound_tls_full":
		data, err = anyTLSOutboundTLSFull()
	case "durations":
		data, err = durations()
	case "anytls_inbound_padding_single":
		data, err = anyTLSInboundPaddingScheme([]string{"pad"})
	case "anytls_inbound_padding_array":
		data, err = anyTLSInboundPaddingScheme([]string{"pad-a", "pad-b", "pad-c"})
	default:
		fmt.Fprintf(os.Stderr, "unknown fixture %q\n", *fixture)
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "marshal:", err)
		os.Exit(1)
	}

	if *out == "-" {
		os.Stdout.Write(data)
		return
	}
	if err := os.WriteFile(*out, data, 0o644); err != nil {
		fmt.Fprintln(os.Stderr, "write:", err)
		os.Exit(1)
	}
}

func anyTLSOutbound() ([]byte, error) {
	opts := &option.AnyTLSOutboundOptions{}
	opts.Server = "example.com"
	opts.ServerPort = 443
	opts.Password = "hunter2"
	opts.TLS = &option.OutboundTLSOptions{
		Enabled:    true,
		ServerName: "example.com",
	}
	opts.ConnectTimeout = badoption.Duration(10 * time.Second)

	outbound := &option.Outbound{
		Type:    "anytls",
		Tag:     "proxy",
		Options: opts,
	}

	return singjson.MarshalContext(context.Background(), outbound)
}

// anyTLSOutboundTLSFull puts every nested TLS object and every non-trivial
// field shape of OutboundTLSOptions on the wire at once - uTLS, REALITY and
// ECH objects, CurvePreference names, a []byte list (base64), Listables of
// one (bare element) and of several (array), durations - so the generated
// OutboundTLSOptions and its children are checked against the real marshaler,
// not just their empty case. (REALITY and ECH together make no working
// config; this is about the JSON shape only.)
func anyTLSOutboundTLSFull() ([]byte, error) {
	opts := &option.AnyTLSOutboundOptions{}
	opts.Server = "example.com"
	opts.ServerPort = 443
	opts.Password = "hunter2"
	opts.TLS = &option.OutboundTLSOptions{
		Enabled:                    true,
		ServerName:                 "www.ebay.com",
		ALPN:                       badoption.Listable[string]{"h2", "http/1.1"},
		MinVersion:                 "1.2",
		CipherSuites:               badoption.Listable[string]{"TLS_AES_128_GCM_SHA256"},
		CurvePreferences:           badoption.Listable[option.CurvePreference]{option.X25519MLKEM768, option.CurveP256},
		CertificatePublicKeySHA256: badoption.Listable[[]byte]{{0x01, 0x02, 0xfe, 0xff}},
		Fragment:                   true,
		FragmentFallbackDelay:      badoption.Duration(500 * time.Millisecond),
		HandshakeTimeout:           badoption.Duration(15 * time.Second),
		ECH: &option.OutboundECHOptions{
			Enabled:         true,
			Config:          badoption.Listable[string]{"-----BEGIN ECH CONFIGS-----", "AEX+DQBB", "-----END ECH CONFIGS-----"},
			QueryServerName: "cloudflare-ech.com",
		},
		UTLS: &option.OutboundUTLSOptions{
			Enabled:     true,
			Fingerprint: "chrome",
		},
		Reality: &option.OutboundRealityOptions{
			Enabled:   true,
			PublicKey: "jNXHt1yRo0vDuchQlIP6Z0ZvjT3KtzVI-T4E7RoLJS0",
			ShortID:   "0123abcd",
		},
	}

	outbound := &option.Outbound{
		Type:    "anytls",
		Tag:     "proxy",
		Options: opts,
	}
	return singjson.MarshalContext(context.Background(), outbound)
}

// durations puts badoption.Duration's two directions through the pinned sing
// code over the edge cases: formatting (time.Duration.String - units below a
// second, long fractions, the int64 extremes) and parsing (my_time.
// ParseDuration via UnmarshalJSON - every unit spelling, fractions, signs,
// overflow, malformed input; an error is a null "ns").
func durations() ([]byte, error) {
	type formatCase struct {
		Ns   int64  `json:"ns"`
		Text string `json:"text"`
	}
	type parseCase struct {
		Text string `json:"text"`
		Ns   *int64 `json:"ns"`
	}
	var result struct {
		Format []formatCase `json:"format"`
		Parse  []parseCase  `json:"parse"`
	}
	for _, ns := range []int64{
		0, 1, 999, 1000, 1100, 999999, 1000000, 1500000, 500000000, 999999999,
		1000000000, 1500000000, 1123456789, 60000000000, 90000000000, 3600000000000,
		3723004005006, 90000000000000, -2000000, -1, math.MaxInt64, math.MinInt64,
	} {
		result.Format = append(result.Format, formatCase{ns, time.Duration(ns).String()})
	}
	for _, text := range []string{
		"0", "+0", "-0", "0s", "1ns", "1us", "1µs", "1μs", "1.1µs", "300ms", "1.5h", "-1.5h",
		"2h45m", "1d", "1.5d", "+5s", ".5s", "1.s", "1h0m0s", "1.123456789s", "1.0000000001s",
		"9223372036854775807ns", "9223372036854775808ns", "-9223372036854775808ns",
		"2562047h", "2562048h", "", "5", "1x", ".s", "s", "-", "1s2", "1.2.3s", "1 s",
	} {
		quoted, err := json.Marshal(text)
		if err != nil {
			return nil, err
		}
		var d badoption.Duration
		c := parseCase{Text: text}
		if d.UnmarshalJSON(quoted) == nil {
			ns := int64(d)
			c.Ns = &ns
		}
		result.Parse = append(result.Parse, c)
	}
	return json.Marshal(result)
}

// anyTLSInboundPaddingScheme exercises badoption.Listable[string]'s two
// wire shapes directly: Go marshals a single-element list as the bare
// element, and any other length as a JSON array (see
// badoption/listable.go) — one fixture per shape.
func anyTLSInboundPaddingScheme(scheme []string) ([]byte, error) {
	opts := &option.AnyTLSInboundOptions{
		PaddingScheme: badoption.Listable[string](scheme),
	}
	return singjson.MarshalContext(context.Background(), opts)
}
