// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// The reviewed tables hand their contents back by copy, and each answers "not
// reviewed" for a program or script it has never heard of. Both matter at the
// admission boundary: a caller that mutated a returned slice would rewrite the
// review itself, and a silent empty answer would read as "this tool parses no
// flags" rather than "this tool was never reviewed".

func TestAnUnreviewedToolStatesNoFlagsRatherThanNone(t *testing.T) {
	for _, program := range []string{"ra8ci:not-a-tool", "", "ascii", "RA8CI:ASCII"} {
		flags, known := ReviewedToolFlags(program)
		if known {
			t.Fatalf("the unreviewed tool %q was reported as reviewed", program)
		}
		if flags != nil {
			t.Fatalf("the unreviewed tool %q was handed flags: %v", program, flags)
		}
	}
}

func TestReviewedToolFlagsAreHandedBackByCopy(t *testing.T) {
	const program = "ra8ci:ascii"
	first, known := ReviewedToolFlags(program)
	if !known || len(first) == 0 {
		t.Fatalf("%s states no reviewed flags: %v %v", program, first, known)
	}
	first[0] = "--rewritten"
	second, _ := ReviewedToolFlags(program)
	if second[0] == "--rewritten" {
		t.Fatalf("mutating the returned flags rewrote the review itself: %v", second)
	}
}

func TestAnUnreviewedScriptStatesNoReportingModes(t *testing.T) {
	for _, script := range []string{"scripts/not-a-script.sh", "", "ci.sh"} {
		modes, stated := ScriptReportingModes(script)
		if stated {
			t.Fatalf("the unreviewed script %q was reported as stating modes", script)
		}
		if modes != nil {
			t.Fatalf("the unreviewed script %q was handed modes: %v", script, modes)
		}
	}
}

func TestScriptReportingModesAreHandedBackByCopy(t *testing.T) {
	const script = "scripts/ci.sh"
	first, stated := ScriptReportingModes(script)
	if !stated || len(first) == 0 {
		t.Fatalf("%s states no reporting modes: %v %v", script, first, stated)
	}
	first[0] = "--rewritten"
	second, _ := ScriptReportingModes(script)
	if second[0] == "--rewritten" {
		t.Fatalf("mutating the returned modes rewrote the review itself: %v", second)
	}
}

// ciScriptMode returns "" wherever another door owns the argv, so that door's
// message survives instead of being replaced by a mode-ignores refusal.
func TestAnArgvAnotherDoorOwnsNamesNoMode(t *testing.T) {
	owned := map[string][]string{
		"a reporting mode":       {"--list-gates"},
		"short help":             {"-h"},
		"long help":              {"--help"},
		"the abort probe":        {"--selftest-abort"},
		"container without gate": {"--container"},
		"no options at all":      {},
	}
	for name, args := range owned {
		if mode := ciScriptMode(args); mode != "" {
			t.Fatalf("%s should be owned by another door, got mode %q", name, mode)
		}
	}
}

func TestTheModesCiScriptDoesName(t *testing.T) {
	cases := map[string]struct {
		args []string
		mode string
	}{
		"a named gate":     {[]string{"--gate", "ascii"}, "single-gate"},
		"the native suite": {[]string{"--native"}, "native suite"},
		// A named gate is judged before the native suite is, so an argv
		// carrying both takes the single-gate branch and the
		// native-despite-a-gate wording is never reached. Pinned as the
		// ordering it is, not the ordering the wording suggests.
		"a gate beside the native suite": {[]string{"--native", "--gate", "ascii"}, "single-gate"},
		"a gate beside a container":      {[]string{"--gate", "ascii", "--container"}, ""},
	}
	for name, test := range cases {
		if mode := ciScriptMode(test.args); mode != test.mode {
			t.Fatalf("%s named mode %q, expected %q", name, mode, test.mode)
		}
	}
}

// A mode nobody declared has no stated reason, and an empty reason is what a
// refusal reads rather than a lie about why.
func TestAnUndeclaredModeHasNoStatedReason(t *testing.T) {
	rules, stated := scriptModesIgnoringOptions["scripts/ci.sh"]
	if !stated {
		t.Fatal("scripts/ci.sh states no ignored options to read a reason from")
	}
	if reason := rules.reasonFor("a mode nobody declared"); reason != "" {
		t.Fatalf("an undeclared mode was given the reason %q", reason)
	}
	named := rules.modes[0].name
	if reason := rules.reasonFor(named); reason == "" {
		t.Fatalf("the declared mode %q states no reason", named)
	}
}

// A ceiling above one is said as a bound rather than as a count, so the
// refusal reads the way the reviewed limit is written.
func TestATargetCeilingIsSaidAsItIsWritten(t *testing.T) {
	if word := targetWord(1); word != "exactly one" {
		t.Fatalf("a ceiling of one reads %q", word)
	}
	for ceiling, want := range map[int]string{2: "at most 2", 7: "at most 7", 0: "at most 0"} {
		if word := targetWord(ceiling); word != want {
			t.Fatalf("a ceiling of %d reads %q, expected %q", ceiling, word, want)
		}
	}
}

// The ignored-option refusal carries the mode's own reason, which is the
// sentence that tells an operator why a passing run did the wrong work.
func TestTheIgnoredOptionRefusalCarriesTheModesReason(t *testing.T) {
	rules := scriptModesIgnoringOptions["scripts/ci.sh"]
	var named, ignored, because string
	for _, mode := range rules.modes {
		if len(mode.ignores) != 0 {
			named, ignored, because = mode.name, mode.ignores[0], mode.because
			break
		}
	}
	if named == "" {
		t.Skip("no ci.sh mode ignores an option to build a refusal from")
	}
	step := Step{Name: "suite", Args: append([]string{"scripts/ci.sh"}, argvSelecting(t, rules, named)...)}
	step.Args = append(step.Args, ignored)
	err := checkNoScriptOptionIsOneTheModeIgnores(step)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("an option the %s mode ignores was not refused: %v", named, err)
	}
	for _, part := range []string{ignored, named, because} {
		if !strings.Contains(err.Error(), part) {
			t.Fatalf("the refusal omits %q: %v", part, err)
		}
	}
}

// argvSelecting finds the shortest argv that makes the rules pick the mode
// named, so the test never hardcodes ci.sh's own flag spellings.
func argvSelecting(t *testing.T, rules scriptModeRules, mode string) []string {
	t.Helper()
	for _, candidate := range [][]string{
		{"--native"},
		{"--gate", "ascii"},
		{"--native", "--gate", "ascii"},
	} {
		if rules.modeFor(candidate) == mode {
			return candidate
		}
	}
	t.Skipf("no known argv selects the %s mode", mode)
	return nil
}
