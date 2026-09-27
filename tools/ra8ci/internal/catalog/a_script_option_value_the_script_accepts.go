// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The shell mirror of a_tool_option_value_the_tool_accepts.go, and the last
// unjudged thing a reviewed step can hand scripts/ci.sh.
//
// Four shell doors stand. They settle which options a script parses, which of
// them report instead of working, which pairs it refuses together, and which
// ones the mode it takes accepts and never reads. Every one of them judges an
// option by its NAME. None reads the VALUE that follows.
//
// For six of ci.sh's nine options that is the whole story: they are valueless
// mode switches and there is no value to judge. Two take one:
//
//   - --gate <name> is checked against the gate registry, which lives in
//     ci.sh and is sixty rows long. DELIBERATELY NOT JUDGED HERE. Restating
//     sixty gate names in Go would be a second registry going stale the first
//     time a gate is added, and the failure is loud anyway: an unregistered
//     gate name exits non-zero with the registry printed beside it.
//   - --selftest-abort <mode> takes exactly three, and that is this door.
//
// The abort probe is the ONE ci.sh option whose value is closed, short and
// unlikely to move: _ci_abort_probe_registry (scripts/ci/lib/abort.sh) is a
// case over hang, destroy and fail, swapping in two fixture gates per mode,
// with a *) arm printing "unknown --selftest-abort mode" and returning 2. The
// reporting-mode door explicitly left the probe admitted, on the ground that
// it does run the real suite runner and its exit reports what the probe found.
// That reasoning holds only while the mode NAMES one of the three registries.
// A step naming a fourth spelling is admitted by review, digested, shipped,
// dispatched, and answers exit 2 on every runner forever, having driven no
// fixture and read nothing.
//
// RESTATED, NOT READ, like every table on this seam, and with the same
// consequence: the value contract of an option lives in the script, review
// runs against an embedded manifest, so the two are kept honest by
// TestEveryJudgedScriptOptionIsOneItsScriptTakesAValueFor rather than by
// reading a checkout.
var reviewedScriptOptionValues = map[string]map[string][]string{
	"scripts/ci.sh": {
		// Read out of _ci_abort_probe_registry's own case arms.
		"--selftest-abort": {"hang", "destroy", "fail"},
	},
}

// ScriptOptionValues returns the values a script's option accepts, and is how
// a caller can tell a closed option from one whose value this seam does not
// judge.
func ScriptOptionValues(script, option string) ([]string, bool) {
	values, stated := reviewedScriptOptionValues[script][option]
	if !stated {
		return nil, false
	}
	return append([]string(nil), values...), true
}

// checkScriptOptionValuesAreOnesTheScriptAccepts refuses a reviewed step
// handing a script an option value the script names in no arm of its own case.
// It reads the element AFTER the option, which is where the script's parser
// shifts to find it; a missing value is the option door's refusal, not this
// one's.
func checkScriptOptionValuesAreOnesTheScriptAccepts(step Step) error {
	if len(step.Args) == 0 {
		return nil
	}
	script := step.Args[0]
	options, stated := reviewedScriptOptionValues[script]
	if !stated {
		return nil
	}
	rest := step.Args[1:]
	for i, arg := range rest {
		values, judged := options[arg]
		if !judged || i+1 >= len(rest) {
			continue
		}
		value := rest[i+1]
		if statesExactly(values, value) {
			continue
		}
		return fmt.Errorf("%w: step %q hands %q the %s value %q, which it names in no arm of its own case and answers with exit 2 having run nothing (it takes: %s)",
			ErrInvalidCatalog, step.Name, script, arg, value, strings.Join(values, ", "))
	}
	return nil
}
