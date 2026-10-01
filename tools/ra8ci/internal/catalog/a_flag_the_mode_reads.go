// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The fourth shell door, and the one the combination door
// (a_container_step_names_a_gate.go) named and declined: the options a script
// PARSES, ACCEPTS, and then never reads, because the mode it took exits first.
//
// The three standing shell doors each judge one thing about an argument. The
// option door settles whether the script parses the spelling at all. The
// reporting-mode door refuses an option that returns zero having done no work.
// The combination door refuses a pair the script rejects outright. All three
// ask what the script REFUSES. None of them can see an argument the script
// accepts into a variable and then walks past.
//
// scripts/ci.sh walks past several. Its parser fills fast, native, container,
// rebuild and gate, and only then picks a mode, in this order:
//
//	if [[ -n "$gate" && "$container" != "1" ]]; then ... exit   # single-gate
//	if [[ "$container" == "1" && -z "$gate" ]]; then ... exit 2 # the pair door
//	if [[ "$native" == "1" ]]; then run_suite_on_snapshot "$fast"; exit
//	ci_host_mode_exec "$fast" "$gate" "$rebuild" ...            # host mode
//
// The single-gate branch reads the gate name and NOTHING ELSE, so
// `--gate misra --fast` runs misra in full; `--gate misra --rebuild` rebuilds
// no image; `--gate misra --native` was already native. The native branch
// hands run_suite_on_snapshot only fast, so `--native --rebuild` rebuilds
// nothing. Host mode is the one mode that reads every flag it was given, and
// so states no ignored options here.
//
// WHY THIS IS WORTH A DOOR WHEN NOTHING FAILS. Every refusal the other shell
// doors make ends in exit 2 and a red step somebody looks at. This one ends
// GREEN: the step asks for a fast run and gets a slow one, or asks for a fresh
// image and gets a stale one, and history files a pass either way. The step's
// own text is the only record of what was wanted, and it is wrong about what
// happened. That is the reporting-mode failure (a gate's verdict that proved
// less than it claims) wearing a passing run instead of an empty one.
//
// THE RULE IS PER MODE, NOT PER FLAG, and this is the whole reason the table
// has the shape it has. --rebuild is read on the host path and ignored on the
// other two. --fast is read everywhere except single-gate. A per-flag table
// would refuse the shipped catalog's own containerised suite runs, which is
// the mistake the scope-selector door (#1998) had to back out of mid-slice.
//
// THE SHARPEST ENTRY IS THE LAST ONE. `--native --gate X --container` clears
// the single-gate branch (container is set), clears the pair door (a gate was
// named), reaches the native branch and runs run_suite_on_snapshot "$fast"
// with NO gate argument at all. The step names one gate and one container and
// gets the WHOLE SUITE on the host, green, with neither honoured.
//
// RESTATED, NOT READ, like every table on this seam: review runs against an
// embedded manifest, not against a checkout, so it cannot ask a script which
// branch it would take.
var scriptModesIgnoringOptions = map[string]scriptModeRules{
	"scripts/ci.sh": {
		modeFor: ciScriptMode,
		modes: []scriptMode{{
			name:    "single-gate",
			ignores: []string{"--fast", "--native", "--rebuild"},
			because: "the single-gate branch runs the named gate in place and exits before any of them is read",
		}, {
			name:    "native suite",
			ignores: []string{"--rebuild"},
			because: "the native branch runs the suite on a snapshot and never builds an image to rebuild",
		}, {
			name:    "native suite despite a named gate",
			ignores: []string{"--rebuild", "--gate", "--container"},
			because: "--container sends a named gate past the single-gate branch, and the native branch then runs the whole suite on the host with no gate argument",
		}},
	},
}

// scriptMode states one branch a script can take and the options that branch
// accepts into a variable and then never reads.
type scriptMode struct {
	// name is how the script's own help calls this mode.
	name string
	// ignores are the exact spellings the mode does not read.
	ignores []string
	// because is the script's own reason, for a refusal that teaches.
	because string
}

// scriptModeRules pairs a script's modes with the reader that says which of
// them an argv selects. The reader is the one script-specific piece: a mode is
// chosen by the order of the script's own branches, which no table can state.
type scriptModeRules struct {
	modeFor func(args []string) string
	modes   []scriptMode
}

// ScriptModeIgnoring reports the options a script's mode accepts and never
// reads, and is how a caller can tell a flag that lands from one that does not.
func ScriptModeIgnoring(script, mode string) ([]string, bool) {
	rules, stated := scriptModesIgnoringOptions[script]
	if !stated {
		return nil, false
	}
	for _, known := range rules.modes {
		if known.name == mode {
			return append([]string(nil), known.ignores...), true
		}
	}
	return nil, false
}

// ciScriptMode names the branch scripts/ci.sh takes for an argv, mirroring the
// order of its own mode guards. It returns "" where another door owns the
// argv, so that door's message survives: the reporting modes and the internal
// abort probe return before a mode is picked at all, --container without a
// gate is the combination door's refusal, and host mode reads every flag it is
// given and has nothing to ignore.
func ciScriptMode(args []string) string {
	for _, owned := range []string{"--list-gates", "-h", "--help", "--selftest-abort"} {
		if namesExactOption(args, owned) {
			return ""
		}
	}
	gate := namesExactOption(args, "--gate")
	container := namesExactOption(args, "--container")
	native := namesExactOption(args, "--native")
	switch {
	case gate && !container:
		return "single-gate"
	case container && !gate:
		return ""
	case native && gate:
		return "native suite despite a named gate"
	case native:
		return "native suite"
	default:
		return ""
	}
}

// checkNoScriptOptionIsOneTheModeIgnores refuses a reviewed step naming an
// option the mode it selects accepts and never reads. Unlike its three
// neighbours this one refuses an argv the script runs happily: the run passes,
// and what it did is not what the step asked for.
func checkNoScriptOptionIsOneTheModeIgnores(step Step) error {
	if len(step.Args) == 0 {
		return nil
	}
	script := step.Args[0]
	rules, stated := scriptModesIgnoringOptions[script]
	if !stated {
		return nil
	}
	rest := step.Args[1:]
	mode := rules.modeFor(rest)
	if mode == "" {
		return nil
	}
	ignored, stated := ScriptModeIgnoring(script, mode)
	if !stated {
		return nil
	}
	for _, option := range ignored {
		if !namesExactOption(rest, option) {
			continue
		}
		return fmt.Errorf("%w: step %q passes %q to %q, which takes its %s mode for this argv: %s, so the step would be recorded as passing having done something other than what it names (unread in that mode: %s)",
			ErrInvalidCatalog, step.Name, option, script, mode, rules.reasonFor(mode), strings.Join(ignored, ", "))
	}
	return nil
}

// reasonFor returns a mode's stated reason, for a refusal that names why the
// option never lands rather than only that it does not.
func (r scriptModeRules) reasonFor(mode string) string {
	for _, known := range r.modes {
		if known.name == mode {
			return known.because
		}
	}
	return ""
}
