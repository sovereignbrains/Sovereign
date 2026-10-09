package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/sagernet/sing-box/common/srs"
	"github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/option"
	singjson "github.com/sagernet/sing/common/json"
)

// A local rule-set in source format - the tray's AdGuard list, ~4 MB of
// JSON - costs sing-box ~1 s to read on every start; a binary one ~0.1 s
// (measured 09.10.2026, sing-box 1.14.2). So each is compiled once, the way
// `sing-box rule-set compile` does, into the service's own folder
// (%ProgramData%\Sovereign\rules - not the source's: a SYSTEM process writes
// nowhere a user picks), named by the source's path, size and time; the box
// reads that. A newer source is compiled at the next start. Anything failing
// leaves the entry as it was: the box reads the source, slower.
func compileSourceRuleSets(options *option.Options) {
	if options.Route == nil {
		return
	}
	dir := filepath.Join(os.Getenv("ProgramData"), "Sovereign", "rules")
	keep := map[string]bool{}
	for i := range options.Route.RuleSet {
		set := &options.Route.RuleSet[i]
		local := set.Type == constant.RuleSetTypeLocal || set.Type == ""
		if !local || set.Format != constant.RuleSetFormatSource {
			continue
		}
		compiled, err := compiledRuleSet(dir, set.LocalOptions.Path)
		if err != nil {
			continue
		}
		set.Format = constant.RuleSetFormatBinary
		set.LocalOptions.Path = compiled
		keep[filepath.Base(compiled)] = true
	}
	// The ones of sources since changed, or gone.
	if entries, err := os.ReadDir(dir); err == nil {
		for _, entry := range entries {
			if strings.HasSuffix(entry.Name(), ".srs") && !keep[entry.Name()] {
				_ = os.Remove(filepath.Join(dir, entry.Name()))
			}
		}
	}
}

// compiledRuleSet: the binary of the source rule-set at `source`, in `dir` -
// made now unless it's there already.
func compiledRuleSet(dir, source string) (string, error) {
	info, err := os.Stat(source)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256([]byte(fmt.Sprintf("%s|%d|%d", source, info.Size(), info.ModTime().UnixNano())))
	target := filepath.Join(dir, hex.EncodeToString(sum[:8])+".srs")
	if _, err := os.Stat(target); err == nil {
		return target, nil
	}
	content, err := os.ReadFile(source)
	if err != nil {
		return "", err
	}
	plain, err := singjson.UnmarshalExtended[option.PlainRuleSetCompat](content)
	if err != nil {
		return "", err
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	partial := target + ".partial"
	file, err := os.Create(partial)
	if err != nil {
		return "", err
	}
	err = srs.Write(file, plain.Options, plain.Version)
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	if err == nil {
		err = os.Rename(partial, target)
	}
	if err != nil {
		_ = os.Remove(partial)
		return "", err
	}
	return target, nil
}
