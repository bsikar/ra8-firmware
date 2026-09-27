// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The seventeenth door on the ra8ci: dispatch seam, and the second that reads
// one argv element against another rather than against the tool's parser.
//
// Every standing tool door judges an option on its own terms: the name the
// tool parses (#1992), the exclusive self test (#1993), a path the tool reads
// (#1994), --all beside a target (#1995), the value the option carries
// (#2000), how many targets the tool takes (#2003), the same flag named twice
// (#2014). Not one of them reads the POSITION an option is written in, and for
// three of the eighteen dispatched tools the position is the whole story.
//
// ascii, runner-clock and tests-readme parse with the standard flag package
// (flag.NewFlagSet then flags.Parse). That parser STOPS at the first argument
// that is not an option and hands everything after it on as a positional:
// flag.Parse walks argv only while the elements look like flags, and the
// remainder is what NArg and Arg report. So the option written after a target
// is not an option at all. It is another target.
//
// `ra8ci:ascii src/a.c --checkout` is the live entry. Both elements are
// beyond reproach one at a time, which is why every door before this one
// admits it: --checkout is a flag ascii parses, src/a.c is a target ascii
// reads, and the step names exactly one of each. What ascii receives is two
// targets and no --checkout, so parseOptions (asciigate.go:137,
// !*all && flags.NArg() != 1) answers "pass one target path, optionally with
// --checkout, or pass --all" and exits 2 before a byte of the tree is read.
// Once per attempt, on every runner, and what history files is a gate result:
// a number an operator reads as this repository failing the ASCII standard,
// out of a step that judged nothing. The reviewed text says one thing and the
// parser does another, and nothing in the catalog could see the difference.
//
// THE OTHER FIFTEEN ARE DELIBERATELY NOT JUDGED. They compare argv against
// the exact forms they accept, scanning every element wherever it sits, so a
// flag written after a path is still found and still read. A uniform rule
// would refuse steps those tools run correctly, which is the mistake the
// scope-selector door (#1998) had to back out of mid-slice.
//
// RESTATED, NOT IMPORTED, for the reason every table on this seam is: the
// executor imports the catalog, so the catalog cannot ask a tool how it
// parses. Each entry below was read out of that tool's own parser this slice.
var toolsStoppingAtTheFirstTarget = map[string]bool{
	"ra8ci:ascii":        true, // asciigate.go:122-130, flag.NewFlagSet then flags.Parse(args)
	"ra8ci:runner-clock": true, // runnerclock.go:89-96, same, and NArg() != 0 is exit 2
	"ra8ci:tests-readme": true, // testsreadme.go:50-56, same, and NArg() != 0 is exit 2
}

// ToolStopsAtTheFirstTarget reports whether a reviewed tool parses with the
// flag package, which stops reading options at the first argument that is not
// one. A caller can use it to tell a tool where argv order carries meaning
// from one where it does not.
func ToolStopsAtTheFirstTarget(program string) bool {
	return toolsStoppingAtTheFirstTarget[program]
}

// checkNoToolFlagFollowsATarget refuses a reviewed step that writes an option
// after a target for a tool whose parser has already stopped reading options
// by then. It is an admission rule, applied where a manifest is read rather
// than against a task already persisted under a reviewed digest, the same line
// every door on this seam draws.
func checkNoToolFlagFollowsATarget(step Step, program string) error {
	if !toolsStoppingAtTheFirstTarget[program] {
		return nil
	}
	target := ""
	expectingValue := false
	for _, arg := range step.Args {
		if expectingValue {
			expectingValue = false
			continue
		}
		if strings.HasPrefix(arg, "-") {
			if target != "" {
				return fmt.Errorf("%w: step %q passes %q to %q after the target %q; that parser stops reading options at the first argument that is not one, so %q arrives as another target and the option review wrote down is never read",
					ErrInvalidCatalog, step.Name, arg, program, target, arg)
			}
			expectingValue = toolFlagTakesAValue(program, flagNameArgvClaims(arg)) && !strings.Contains(arg, "=")
			continue
		}
		if target == "" {
			target = arg
		}
	}
	return nil
}
