// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The argv half of the ra8ci: dispatch seam, and the companion of
// tool_programs.go.
//
// checkToolProgramExists closed the NAME: a reviewed step naming a tool the
// executor does not implement fails on every runner in the fleet, so review
// refuses it rather than letting one attempt per runner discover it. The
// ARGUMENTS it passes that tool were still judged only as text
// (validateDispatchArgs: non-empty, inside MaxArgumentValueBytes, printable
// ASCII). Nothing asked whether the tool parses them.
//
// Every tool the executor dispatches in process reads its own argv, and each
// one refuses what it does not recognise: the flag-package tools (ascii,
// runner-clock, tests-readme) return a usage error from flags.Parse, and the
// rest compare argv against the exact forms they accept and print usage. All
// of them then exit 2. So a reviewed step dispatching ra8ci:legacy-make --all,
// or the plain typo --sefltest, was admitted by review, digested into the
// catalog, dispatched to a guest, and came back exit 2 with a usage line on
// the step's stderr, once per attempt, on every runner, forever. The gate
// never read the tree, and what history files is a gate result: a number an
// operator reads as this repository violating the standard, out of a step that
// judged nothing.
//
// wave-references fails the other way and is worth naming, because it is the
// reason this door is not only about exit codes: it looks for --selftest
// anywhere in argv and IGNORES every other argument. A reviewed step passing
// it a flag it does not read is admitted, runs green, and files a verdict that
// is not the one review believed it was approving.
//
// THE SET IS RESTATED, NOT IMPORTED, for the same reason the tool names are:
// the executor imports the catalog, so the catalog cannot ask the executor
// what its tools parse. Each entry below is the flag set of that tool's own
// parser (internal/<tool>.Run), and
// TestEveryReviewedToolStatesTheFlagsItParses holds the table to the dispatch
// list so a tool cannot be added in one place and forgotten in the other.
//
// DELIBERATELY ONLY THE FLAG-SHAPED ARGUMENTS. A tool that takes file
// arguments (ascii, assert-casts, final-newline, gnu-attribute, no-null,
// since, tz-boundary-discard) resolves them against the checkout it was handed,
// and whether a path exists there is the checkout's question, not review's.
// This door judges what argv CLAIMS to be an option, which is the half review
// can settle on its own.
var reviewedToolFlags = map[string][]string{
	"ra8ci:ascii":                         {"all", "check", "checkout", "selftest"},
	"ra8ci:assert-casts":                  {"all", "selftest"},
	"ra8ci:driver-asm-guard":              {"selftest"},
	"ra8ci:final-newline":                 {"all", "selftest"},
	"ra8ci:gnu-attribute":                 {"selftest"},
	"ra8ci:inclusive-terminology-commits": {"selftest"},
	"ra8ci:legacy-make":                   {"selftest"},
	"ra8ci:no-goto-setjmp":                {"selftest"},
	"ra8ci:no-null":                       {"all", "selftest"},
	"ra8ci:no-unsafe-python-install":      {"selftest"},
	"ra8ci:nsc-veneer-defs":               {"selftest"},
	"ra8ci:pointer-boilerplate":           {"selftest"},
	"ra8ci:runner-clock":                  {"ci-scan", "hours", "repo", "runs", "selftest"},
	"ra8ci:since":                         {"all", "selftest"},
	"ra8ci:stub-crypto-guard":             {"selftest"},
	"ra8ci:tests-readme":                  {"selftest"},
	"ra8ci:tz-boundary-discard":           {"selftest"},
	"ra8ci:wave-references":               {"selftest"},
}

// ReviewedToolFlags returns the flags a reviewed ra8ci: tool parses.
func ReviewedToolFlags(program string) ([]string, bool) {
	accepted, known := reviewedToolFlags[program]
	if !known {
		return nil, false
	}
	return append([]string(nil), accepted...), true
}

// checkToolFlagsAreOnesTheToolParses refuses a reviewed step that hands an
// ra8ci: tool an option the tool does not read. It is an admission rule,
// applied where a manifest is read rather than against a task already
// persisted under a reviewed digest, the same line checkToolProgramExists
// draws.
func checkToolFlagsAreOnesTheToolParses(step Step, program string) error {
	accepted, known := reviewedToolFlags[program]
	if !known {
		return fmt.Errorf("%w: step %q names the tool %q, which states no reviewed flags; a dispatched tool must state the options it parses",
			ErrInvalidCatalog, step.Name, program)
	}
	for _, arg := range step.Args {
		if !strings.HasPrefix(arg, "-") {
			continue
		}
		name := flagNameArgvClaims(arg)
		if name == "" {
			return fmt.Errorf("%w: step %q passes %q to %q, which is a dash naming no option; no reviewed step needs one to end option parsing",
				ErrInvalidCatalog, step.Name, arg, program)
		}
		if !statesFlag(accepted, name) {
			return fmt.Errorf("%w: step %q passes the option %q to %q, which parses only %s; every runner would answer it with a usage error and no reading of the tree",
				ErrInvalidCatalog, step.Name, arg, program, strings.Join(dashed(accepted), ", "))
		}
	}
	return nil
}

// flagNameArgvClaims returns the option name an argument claims, with the
// leading dashes and any =value tail removed. Both -flag and --flag are read,
// because the flag package accepts either and the tools comparing argv exactly
// accept the long form.
func flagNameArgvClaims(arg string) string {
	name := strings.TrimPrefix(strings.TrimPrefix(arg, "-"), "-")
	if index := strings.IndexByte(name, '='); index >= 0 {
		name = name[:index]
	}
	return name
}

func statesFlag(accepted []string, name string) bool {
	for _, candidate := range accepted {
		if candidate == name {
			return true
		}
	}
	return false
}

func dashed(names []string) []string {
	shown := make([]string, 0, len(names))
	for _, name := range names {
		shown = append(shown, "--"+name)
	}
	return shown
}
