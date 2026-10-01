// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The argv half of the SHELL dispatch, and the companion of the argv doors on
// the ra8ci: half.
//
// A reviewed step reaches its work through one of two shapes. The tool shape
// now has its argv fully stated: the program name, the option names, the
// option values, the exclusive self test, file arguments, whole-tree scans and
// target counts are all judged before a manifest is admitted. The SHELL shape
// was judged only as far as its first argument: ValidScriptPath settles that
// the step dispatches a relative, metacharacter-free .sh path inside the
// verified checkout, and validateDispatchArgs settles that the rest is
// non-empty printable ASCII. Nothing asked whether the script parses it.
//
// scripts/ci.sh is where that gap costs something. It is the single definition
// of every CI gate in this repository and it carries nearly every shell step
// in the shipped catalog. Its parser is a `case` over EXACT argv spellings
// with a `*)` arm that answers anything else with "ci.sh: unknown flag" and
// exit 2, before a gate runs. So a reviewed step dispatching --gates format,
// or the single-dash -fast, was admitted by review, digested, dispatched to a
// guest, and came back exit 2 with a usage block on the step's stderr, once
// per attempt, on every runner, forever, having read nothing.
//
// TWO SPELLINGS OF THE SAME MISTAKE ARE WORSE THAN AN EXIT CODE, and they are
// why this door reads argv exactly rather than through flagNameArgvClaims:
//
//   - `--gate` as the last argument is caught by the script ("--gate requires
//     a gate name", exit 2), but `--gate=` is NOT. It sets gate to the empty
//     string, the single-gate branch tests [[ -n "$gate" ]] and falls through,
//     and the run becomes the WHOLE SUITE. A step that meant one gate silently
//     files the verdict of eighty.
//   - `--gate --fast` is not a parse error either. The script shifts and takes
//     the next element literally, so the gate name becomes "--fast", and what
//     history records is an unknown-gate failure for a gate nobody named.
//
// THE SPELLINGS ARE READ EXACTLY, NOT NORMALISED. The tool doors accept -flag
// and --flag alike because Go's flag package does and the exact-comparison
// tools take the long form. A shell `case` does neither: "-fast" matches no
// arm of ci.sh's parser and reaches `*)`. Normalising here would admit the one
// spelling the script refuses.
//
// THE TABLE IS RESTATED, NOT READ, for the reason every table on this seam is:
// review runs against an embedded manifest, not against a checkout, so it
// cannot ask a script what it parses. Each entry below is read out of that
// script's own argument loop.
//
// ONLY THE SCRIPTS WHOSE CONTRACT IS STATED ARE JUDGED. A script absent from
// the table is admitted on its path alone, exactly as before; the door adds a
// rule where one is known rather than inventing a uniform shell contract that
// no script signed up to.
type scriptOptions struct {
	// valueless are the exact spellings the script's parser accepts alone.
	valueless []string
	// takingTheNextArgument are the exact spellings that consume the
	// following argv element as their value.
	takingTheNextArgument []string
	// takingAnEqualsValue are the spellings whose parser also has a
	// --name=value arm. A spelling missing here has no such arm, and the
	// =form reaches the unknown-flag arm instead.
	takingAnEqualsValue []string
}

// reviewedScriptOptions holds the argument contract of each reviewed script
// the catalog dispatches, read out of that script's own parser.
var reviewedScriptOptions = map[string]scriptOptions{
	// scripts/ci.sh, the gate driver. Read out of its `while [[ $# -gt 0 ]]`
	// case: --fast, --native, --container, --rebuild and --list-gates are
	// flags; -h and --help print usage; --gate and --selftest-abort shift
	// and take the next element; only --gate has a --gate=* arm.
	"scripts/ci.sh": {
		valueless:             []string{"--fast", "--native", "--container", "--rebuild", "--list-gates", "-h", "--help"},
		takingTheNextArgument: []string{"--gate", "--selftest-abort"},
		takingAnEqualsValue:   []string{"--gate"},
	},
}

// ReviewedScriptStatesItsOptions reports whether a script's argument contract
// is stated, and is how a caller can tell "admitted because it is right" from
// "admitted because nothing is known about it".
func ReviewedScriptStatesItsOptions(script string) bool {
	_, stated := reviewedScriptOptions[script]
	return stated
}

// checkScriptOptionsAreOnesTheScriptParses refuses a reviewed step that hands
// a dispatched script an argument its parser does not accept. It is an
// admission rule, applied where a manifest is read rather than against a task
// already persisted under a reviewed digest, the same line every other door on
// this seam draws.
func checkScriptOptionsAreOnesTheScriptParses(step Step) error {
	if len(step.Args) == 0 {
		return nil
	}
	script := step.Args[0]
	stated, known := reviewedScriptOptions[script]
	if !known {
		return nil
	}
	rest := step.Args[1:]
	for index := 0; index < len(rest); index++ {
		arg := rest[index]
		switch {
		case statesExactly(stated.valueless, arg):
			continue
		case statesExactly(stated.takingTheNextArgument, arg):
			if err := checkValueAfter(step, script, stated, arg, rest, index); err != nil {
				return err
			}
			index++
		default:
			name, value, split := strings.Cut(arg, "=")
			if !split || !statesExactly(stated.takingAnEqualsValue, name) {
				return fmt.Errorf("%w: step %q passes %q to %q, which parses only %s; every runner would answer it with a usage error and no gate run",
					ErrInvalidCatalog, step.Name, arg, script, strings.Join(scriptSpellings(stated), ", "))
			}
			if value == "" {
				return fmt.Errorf("%w: step %q passes %q to %q, which reads an empty value as no value at all and runs the whole suite instead of the one thing the step named",
					ErrInvalidCatalog, step.Name, arg, script)
			}
		}
	}
	return nil
}

// checkValueAfter judges the value a value-taking spelling consumes, refusing
// the two shapes the script cannot report: a missing value, and a value that
// is itself one of the script's options, which the parser takes literally.
func checkValueAfter(step Step, script string, stated scriptOptions, arg string, rest []string, index int) error {
	if index+1 >= len(rest) {
		return fmt.Errorf("%w: step %q ends with %q, which %q reads as a missing value and refuses before running anything",
			ErrInvalidCatalog, step.Name, arg, script)
	}
	value := rest[index+1]
	if statesExactly(stated.valueless, value) || statesExactly(stated.takingTheNextArgument, value) {
		return fmt.Errorf("%w: step %q passes %q the option %q as its value, which %q takes literally and then fails naming a value nobody chose",
			ErrInvalidCatalog, step.Name, arg, value, script)
	}
	return nil
}

// statesExactly reports whether a spelling is one the script's parser matches.
// Exact, because a shell case arm is.
func statesExactly(spellings []string, arg string) bool {
	for _, spelling := range spellings {
		if arg == spelling {
			return true
		}
	}
	return false
}

// scriptSpellings names everything the script parses, for a refusal that says
// what would have worked.
func scriptSpellings(stated scriptOptions) []string {
	spellings := append([]string(nil), stated.valueless...)
	for _, spelling := range stated.takingTheNextArgument {
		spellings = append(spellings, spelling+" <value>")
	}
	return spellings
}
