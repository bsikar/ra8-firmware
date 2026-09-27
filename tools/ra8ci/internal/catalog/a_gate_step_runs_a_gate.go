// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The companion of a_script_option_the_script_parses.go, and the one mistake
// on the shell half that costs more than an exit code.
//
// That door settles which arguments a script PARSES. Every refusal it makes
// ends the same way: the script answers with a usage error, exits non-zero,
// and history files a failure. Loud, wrong, and unmissable.
//
// Some options a script parses perfectly well are still ones no reviewed step
// may name, because they make the script report ABOUT its work instead of
// doing it, and then exit ZERO. scripts/ci.sh has three: --list-gates dumps
// the registry and exits with the dump's status, -h and --help print usage and
// exit 0. Each arm sits before the single-gate branch, so it returns having
// read nothing of the tree.
//
// A reviewed step naming one of them is admitted, dispatched, and comes back
// 0. The executor records a PASS. The task's verdict says the gate it names is
// green on that runner, and the gate never ran. That is the wave-references
// failure again, arriving through the shell instead of a tool, and it is worse
// than the exit-2 kind: a red step gets looked at, a green one that proved
// nothing does not.
//
// DELIBERATELY NOT --selftest-abort. It is INTERNAL and its output is a probe
// rather than a suite verdict, but it does run the real suite runner over
// fixture gates and its exit reports what the probe found, so refusing it here
// would be this door making a scheduling judgement rather than naming an
// option that reports instead of works. The option door already holds its
// spelling.
//
// DELIBERATELY NOT a rule about --container either, which is a different
// mistake with a different message: ci.sh refuses --container with no gate
// name outright ("it needs a gate name", exit 2), so it belongs with the
// combination rules, not here.
//
// THE TABLE IS RESTATED, NOT READ, for the reason every table on this seam is:
// review runs against an embedded manifest, not a checkout.
var scriptReportingModes = map[string][]string{
	// Read out of ci.sh's own parser: --list-gates calls list_gates and
	// exits with its status; -h and --help print usage and exit 0. All
	// three return before a gate runs.
	"scripts/ci.sh": {"--list-gates", "-h", "--help"},
}

// ScriptReportingModes returns the options that make a reviewed script report
// instead of work.
func ScriptReportingModes(script string) ([]string, bool) {
	modes, stated := scriptReportingModes[script]
	if !stated {
		return nil, false
	}
	return append([]string(nil), modes...), true
}

// checkAScriptStepRunsTheWorkItNames refuses a reviewed step that names an
// option returning zero without doing the work the step was scheduled for. It
// is an admission rule, applied where a manifest is read rather than against a
// task already persisted under a reviewed digest.
func checkAScriptStepRunsTheWorkItNames(step Step) error {
	if len(step.Args) == 0 {
		return nil
	}
	script := step.Args[0]
	modes, stated := scriptReportingModes[script]
	if !stated {
		return nil
	}
	for _, arg := range step.Args[1:] {
		if !statesExactly(modes, arg) {
			continue
		}
		return fmt.Errorf("%w: step %q names %q, which makes %q report and exit without running a gate; the step would be recorded as passing having read nothing (%s)",
			ErrInvalidCatalog, step.Name, arg, script, strings.Join(modes, ", "))
	}
	return nil
}
