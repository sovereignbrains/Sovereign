package main

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/sagernet/sing-box/common/srs"
)

// A source rule-set compiled once: the binary reads back to the same rules,
// and a second ask finds it made.
func TestCompiledRuleSet(t *testing.T) {
	dir := t.TempDir()
	source := filepath.Join(dir, "sov-ads-adguard.json")
	text := `{"version":3,"rules":[{"type":"logical","mode":"and","rules":[` +
		`{"domain_suffix":["ads.example.com","tracker.net"]},{"domain_suffix":["ok.ads.example.com"],"invert":true}]}]}`
	if err := os.WriteFile(source, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	cache := filepath.Join(dir, "cache")
	compiled, err := compiledRuleSet(cache, source)
	if err != nil {
		t.Fatalf("compile: %v", err)
	}
	file, err := os.Open(compiled)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	read, err := srs.Read(file, false)
	if err != nil {
		t.Fatalf("read back: %v", err)
	}
	if len(read.Options.Rules) != 1 || read.Options.Rules[0].LogicalOptions.Mode != "and" {
		t.Fatalf("rules: %+v", read.Options.Rules)
	}
	stat, _ := os.Stat(compiled)
	again, err := compiledRuleSet(cache, source)
	if err != nil || again != compiled {
		t.Fatalf("again: %q %v", again, err)
	}
	if stat2, _ := os.Stat(again); !stat2.ModTime().Equal(stat.ModTime()) {
		t.Fatal("made twice")
	}
	if _, err := compiledRuleSet(cache, filepath.Join(dir, "missing.json")); err == nil {
		t.Fatal("a missing source compiled")
	}
}
