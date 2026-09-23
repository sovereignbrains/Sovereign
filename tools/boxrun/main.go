// Command boxrun starts a sing-box instance from a JSON config file using
// the pinned vendored sing-box (see tools/codegen/sing-box.version), the
// same option-parsing and registry setup (include.Context) the real
// `sing-box run -c` CLI uses. It exists to stand up local sing-box
// instances — an anytls server, say — for Sovereign's own manual/scripted
// integration checks (see tests/manual); it is dev/test tooling, not
// something Sovereign ships.
//
// It never exits on its own: the caller is expected to terminate the
// process once done with it (TerminateProcess/taskkill), not send it a
// signal — Windows console signal delivery to a child process spawned
// without its own console is unreliable, so graceful shutdown isn't worth
// the complexity here.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/option"
	singjson "github.com/sagernet/sing/common/json"
)

func main() {
	configPath := flag.String("config", "", "path to a sing-box JSON config file")
	flag.Parse()
	if *configPath == "" {
		fmt.Fprintln(os.Stderr, "boxrun: -config is required")
		os.Exit(2)
	}

	data, err := os.ReadFile(*configPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, "boxrun: read config:", err)
		os.Exit(1)
	}

	ctx := include.Context(context.Background())
	var options option.Options
	if err := singjson.UnmarshalContext(ctx, data, &options); err != nil {
		fmt.Fprintln(os.Stderr, "boxrun: parse config:", err)
		os.Exit(1)
	}

	instance, err := box.New(box.Options{Context: ctx, Options: options})
	if err != nil {
		fmt.Fprintln(os.Stderr, "boxrun: create box:", err)
		os.Exit(1)
	}
	if err := instance.Start(); err != nil {
		fmt.Fprintln(os.Stderr, "boxrun: start box:", err)
		os.Exit(1)
	}

	fmt.Println("ready")
	select {}
}
