// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The COMBINATION rule on the shell half, and the mistake the reporting-mode
// door explicitly declined as "a different mistake with a different message".
//
// Two shell doors already stand. One settles which arguments a script parses
// (a_script_option_the_script_parses.go); the other refuses the options it
// parses perfectly and then returns zero having done no work
// (a_gate_step_runs_a_gate.go). Between them sits a third shape: two options a
// script parses individually, which it refuses TOGETHER.
//
// scripts/ci.sh has exactly one, and it is not a parse error, so neither
// standing door can see it. --container is accepted by the argument loop and
// sets a variable. The refusal comes hundreds of lines later, after the
// single-gate branch declines to fire:
//
//	if [[ "$container" == "1" && -z "$gate" ]]; then
//	  echo "ci.sh: --container selects how --gate runs; it needs a gate name."
//	  exit 2
//
// So ra8ci's shell step `bash scripts/ci.sh --container` is admitted by
// review, digested, shipped, dispatched into a guest, and answers exit 2 on
// every runner forever, having built nothing and run nothing. The script's own
// message says why: --container is not a mode, it selects HOW --gate runs, and
// the whole suite is containerised by default already, so the flag alone asks
// for the thing that would have happened anyway and refuses rather than guess.
//
// THE PAIR IS WHAT IS WRONG, NOT EITHER HALF. --container beside a gate name
// is the supported toolchain-image path and is admitted here. --gate alone is
// the CI path. Only the one without the other is refused, which is why this is
// its own door and not an entry in either neighbour's table: those tables
// judge one argv element at a time.
//
// DELIBERATELY NOT JUDGED: the flags a single-gate run SILENTLY IGNORES.
// --fast, --native and --rebuild all reach the single-gate branch, which exits
// before any of them is read, so `--gate X --fast` runs gate X in full rather
// than fast. That is a step asking for something it does not get, which is
// real, but the gate it names still runs and the verdict it files is the
// verdict of that gate. A refusal there would be this door judging what a step
// MEANT; this one only judges what the script refuses outright. Worth its own
// slice, with its own message, if it is judged at all.
//
// RESTATED, NOT READ, like every table on this seam: review runs against an
// embedded manifest, not against a checkout, so it cannot ask a script which
// of its options need a companion.
var scriptOptionsNeedingACompanion = map[string][]optionCompanion{
	// Read out of ci.sh's own body: --container sets container=1 in the
	// argument loop and is refused at the container-without-gate guard
	// unless a gate name was named too.
	"scripts/ci.sh": {{
		option:    "--container",
		companion: "--gate",
		because:   "--container selects how --gate runs; the whole suite is containerised by default",
	}},
}

// optionCompanion states that one option of a script is meaningless, and
// refused, without another.
type optionCompanion struct {
	// option is the exact spelling that needs a companion.
	option string
	// companion is the exact spelling it needs, in the bare form; the
	// --name=value spelling of the same option counts as naming it.
	companion string
	// because is the script's own reason, for a refusal that teaches.
	because string
}

// ScriptOptionCompanion reports the companion a script's option needs, and is
// how a caller can tell an option that stands alone from one that does not.
func ScriptOptionCompanion(script, option string) (string, bool) {
	for _, pair := range scriptOptionsNeedingACompanion[script] {
		if pair.option == option {
			return pair.companion, true
		}
	}
	return "", false
}

// checkAScriptStepNamesEveryCompanionItNeeds refuses a reviewed step naming an
// option that the script accepts alone at parse time and then refuses at run
// time for arriving without its companion.
func checkAScriptStepNamesEveryCompanionItNeeds(step Step) error {
	if len(step.Args) == 0 {
		return nil
	}
	script := step.Args[0]
	pairs, stated := scriptOptionsNeedingACompanion[script]
	if !stated {
		return nil
	}
	rest := step.Args[1:]
	for _, pair := range pairs {
		if !namesExactOption(rest, pair.option) || namesExactOption(rest, pair.companion) {
			continue
		}
		return fmt.Errorf("%w: step %q passes %q to %q without %s: %s, so the script refuses the pair before building or running anything",
			ErrInvalidCatalog, step.Name, pair.option, script, pair.companion, pair.because)
	}
	return nil
}

// namesExactOption reports whether argv names an option, in the bare spelling
// or the --name=value one. Exact on the name, because a shell case arm is:
// -container matches no arm of ci.sh's parser and is refused a door earlier.
func namesExactOption(args []string, option string) bool {
	for _, arg := range args {
		if arg == option {
			return true
		}
		if name, _, split := strings.Cut(arg, "="); split && name == option {
			return true
		}
	}
	return false
}
