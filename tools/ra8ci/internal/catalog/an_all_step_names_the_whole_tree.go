// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The second exclusive mode of a reviewed tool step, and the companion of
// a_selftest_step_names_nothing_else.go.
//
// --selftest says "prove the detector, then exit". --all says "derive the
// first-party file set and scan the whole thing". Both are answers to the
// question of SCOPE, so neither leaves room for a path beside it, and every
// tool that parses --all says so in its own way:
//
//	ascii           parseOptions errors on *all && flags.NArg() != 0
//	assert-casts    reads --all only as len(args) == 1, then refuses any "-" arg
//	final-newline   all is len(args) == 1; !all && contains(args, "--all") is usage
//	no-null         reads --all only as len(args) == 1, else the per-file branch
//	since           reads --all only as len(args) == 1, else usage
//
// All of them exit 2 without scanning anything. So a reviewed step dispatching
// ra8ci:no-null --all src/ra8_batt.c was admitted by review, digested into the
// catalog, dispatched to a guest, and answered with a usage line, once per
// attempt, on every runner, forever. It is the same failure the self-test door
// closes, reached by the other mode switch, and the two doors are worth
// keeping separate because the reasons differ: the self test proves the
// detector and reads no tree at all, while --all reads the WHOLE tree and a
// path beside it is a contradiction about scope rather than about mode.
//
// ascii is the one tool with a legitimate companion: --check turns its rewrite
// into a report and is orthogonal to scope, so --all --check is a real
// reviewed combination. --checkout is not, because it names one file beneath
// the verified checkout, which is the opposite of the whole tree, and
// parseOptions refuses the pair outright.
//
// THE SET IS RESTATED, NOT IMPORTED, for the same reason as the tool names,
// their flags, and their file arguments: the executor imports the catalog.
// TestEveryAllCompanionIsOneItsToolParses holds these names to the option
// table so a renamed flag cannot quietly become a companion nothing reads.
const allFlag = "all"

// allModeCompanions names, per tool, the options that may sit beside --all.
// Only ascii has one.
var allModeCompanions = map[string]map[string]bool{
	"ra8ci:ascii": {"check": true},
}

// checkAnAllStepNamesTheWholeTree refuses a reviewed step that asks a tool for
// the whole tree and then names something beside it. It is an admission rule,
// applied where a manifest is read rather than against a task already
// persisted under a reviewed digest.
func checkAnAllStepNamesTheWholeTree(step Step, program string) error {
	if !namesFlag(step.Args, allFlag) {
		return nil
	}
	companions := allModeCompanions[program]
	for _, arg := range step.Args {
		if !strings.HasPrefix(arg, "-") {
			return fmt.Errorf("%w: step %q asks %q for the whole tree and names the path %q beside it; --all derives its own scope, so dispatch the path as its own step",
				ErrInvalidCatalog, step.Name, program, arg)
		}
		name := flagNameArgvClaims(arg)
		if name == allFlag || companions[name] {
			continue
		}
		return fmt.Errorf("%w: step %q asks %q for the whole tree and passes %q beside it, which that tool reads only on its own; dispatch it as a separate step",
			ErrInvalidCatalog, step.Name, program, arg)
	}
	return nil
}

// namesFlag reports whether argv claims the named option anywhere, reading
// -flag, --flag and --flag=value alike.
func namesFlag(args []string, name string) bool {
	for _, arg := range args {
		if strings.HasPrefix(arg, "-") && flagNameArgvClaims(arg) == name {
			return true
		}
	}
	return false
}
