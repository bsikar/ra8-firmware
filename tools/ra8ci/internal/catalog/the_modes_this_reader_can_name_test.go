// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "testing"

// The mode reader and the table of ignored options are two halves of one
// review, and nothing in the types makes them agree. checkNoScript-
// OptionIsOneTheModeIgnores asks the reader which mode an argv selects and
// then asks the table what that mode ignores; where the table has never
// heard of the mode, the review returns nil and the step is admitted. So a
// branch added to the reader without a row beside it does not fail loudly,
// it quietly stops refusing, which is the failure a review cannot afford.
//
// These two tests hold the halves against each other over every argv the
// reader distinguishes, so the drift is caught at the seam rather than by
// a step that silently passes years later.

// ciScriptFlags are the spellings ciScriptMode reads. A mode is chosen by
// which of them are present, so every combination of them is every argv the
// reader can tell apart.
var ciScriptFlags = []string{
	"--list-gates", "-h", "--help", "--selftest-abort",
	"--gate", "--container", "--native", "--fast", "--rebuild",
}

// argvCombinations returns every subset of the flags, which is 512 argvs and
// costs nothing to walk.
func argvCombinations(flags []string) [][]string {
	combinations := make([][]string, 0, 1<<len(flags))
	for mask := 0; mask < 1<<len(flags); mask++ {
		argv := []string{}
		for index, flag := range flags {
			if mask&(1<<index) != 0 {
				argv = append(argv, flag)
			}
		}
		combinations = append(combinations, argv)
	}
	return combinations
}

// Every mode the reader can name is one the table states. A name the table
// has never heard of would make the review admit a step that passes the
// script an option the script never reads, which is the exact thing this
// door exists to refuse.
func TestEveryModeTheReaderNamesIsOneTheTableStates(t *testing.T) {
	rules, stated := scriptModesIgnoringOptions["scripts/ci.sh"]
	if !stated {
		t.Fatal("scripts/ci.sh states no ignored options")
	}
	for _, argv := range argvCombinations(ciScriptFlags) {
		mode := rules.modeFor(argv)
		if mode == "" {
			continue
		}
		if _, held := ScriptModeIgnoring("scripts/ci.sh", mode); !held {
			t.Fatalf("argv %v selects mode %q, which no row states, so the door would admit whatever that mode ignores", argv, mode)
		}
		if reason := rules.reasonFor(mode); reason == "" {
			t.Fatalf("mode %q is stated but gives no reason, so its refusal would not say why", mode)
		}
	}
}

// And the other direction: every row is a mode some argv actually selects.
// A row no reader can reach is a refusal that will never fire, which reads
// in the table as though the option were covered when it is not.
func TestEveryStatedModeIsOneSomeArgvSelects(t *testing.T) {
	rules, stated := scriptModesIgnoringOptions["scripts/ci.sh"]
	if !stated {
		t.Fatal("scripts/ci.sh states no ignored options")
	}
	reached := map[string]bool{}
	for _, argv := range argvCombinations(ciScriptFlags) {
		if mode := rules.modeFor(argv); mode != "" {
			reached[mode] = true
		}
	}
	for _, known := range rules.modes {
		if !reached[known.name] {
			t.Fatalf("no argv selects the stated mode %q, so nothing it ignores is ever refused", known.name)
		}
	}
}
