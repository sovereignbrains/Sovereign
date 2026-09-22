// Command goldengen produces golden JSON fixtures by marshaling real
// sing-box option types through sing-box's own JSON machinery (pinned
// version — see tools/codegen/sing-box.version). The C++ side unmarshals
// this fixture and re-marshals it; the two must be structurally identical.
// This is what "golden" means here: the reference is the real upstream
// marshaler, not our guess at its behavior.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"

	"github.com/sagernet/sing-box/option"
	singjson "github.com/sagernet/sing/common/json"
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

	outbound := &option.Outbound{
		Type:    "anytls",
		Tag:     "proxy",
		Options: opts,
	}

	return singjson.MarshalContext(context.Background(), outbound)
}
