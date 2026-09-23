// Command httptarget serves a fixed response body on a fixed address. It
// stands in for "the internet" in Sovereign's manual proxy smoke tests
// (see tests/manual) — something a proxied HTTP request can be pointed at
// without depending on real internet access for a deterministic result.
// Dev/test tooling, not part of Sovereign itself.
package main

import (
	"flag"
	"fmt"
	"net/http"
	"os"
)

func main() {
	listen := flag.String("listen", "127.0.0.1:9000", "address to listen on")
	body := flag.String("body", "httptarget", "response body to serve for every request")
	flag.Parse()

	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, *body)
	})

	fmt.Println("ready")
	if err := http.ListenAndServe(*listen, nil); err != nil {
		fmt.Fprintln(os.Stderr, "httptarget:", err)
		os.Exit(1)
	}
}
