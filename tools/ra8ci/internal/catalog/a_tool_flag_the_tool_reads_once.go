// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The tool mirror of an_option_the_script_reads_once.go, and the same mistake
// one parser over.
//
// The shell door refuses a step naming --gate twice because ci.sh's case loop
// assigns one variable and keeps the last. Go's flag package does exactly the
// same thing: each occurrence calls the flag's Set, so
//
//	ra8ci:runner-clock --runs 250 --runs 5
//
// scans FIVE runs. The step says 250 and the plane runs 5, nothing fails, and
// the report filed is a real report over a tenth of the window review asked
// for. runner-clock is also the only dispatched tool that reaches the network,
// so the wrong number is a wrong answer about real CI rather than a usage
// line somebody reads.
//
// No standing tool door can see it. The flag door
// (a_tool_flag_the_tool_parses.go) judges each NAME against the tool's parser
// and both of these are the same good name. The value door
// (a_tool_option_value_the_tool_accepts.go) judges each VALUE and both of
// these are values runner-clock accepts. The collision rule in
// args_collision.go reads a reviewed argument against a DECLARED flag name,
// never against another reviewed argument. So the pair is wrong and every
// door judging one element at a time is right.
//
// REFUSED: a repeat where either occurrence carries a value, since only then
// can the two disagree and only then is something discarded. That covers the
// three flags runner-clock parses with a value (--repo, --runs, --hours, in
// either the =form or the next-argument form), and it covers a boolean given
// an explicit value, because --all --all=false is true and then false and the
// second silently undoes the first.
//
// ADMITTED: a bare switch named twice. --all --all sets the same flag true
// twice and discards nothing, the same call the shell door makes about
// --fast --fast. Pinned by TestABareSwitchNamedTwiceIsNotRefusedHere.
//
// The name is read through flagNameArgvClaims, so -runs, --runs and --runs=5
// are one flag, exactly as the collision rule and the argv doors already read
// them. A tool absent from reviewedToolFlags states no contract and is
// admitted on its name alone.
func checkNoToolFlagIsNamedTwice(step Step, program string) error {
	if len(reviewedToolFlags[program]) == 0 {
		return nil
	}
	seen := make(map[string]string, len(step.Args))
	carried := make(map[string]bool, len(step.Args))
	for index := 0; index < len(step.Args); index++ {
		arg := step.Args[index]
		if !strings.HasPrefix(arg, "-") {
			continue
		}
		name := flagNameArgvClaims(arg)
		spelling, carries := arg, strings.Contains(arg, "=")
		if !carries && toolFlagTakesAValue(program, name) && index+1 < len(step.Args) {
			index++
			spelling, carries = arg+" "+step.Args[index], true
		}
		first, repeated := seen[name]
		if repeated && (carries || carried[name]) {
			return fmt.Errorf("%w: step %q passes --%s to %q twice, as %q and then %q; the flag package calls the same flag both times and keeps the last, so the step would run with the second and drop the first without saying so",
				ErrInvalidCatalog, step.Name, name, program, first, spelling)
		}
		if !repeated {
			seen[name], carried[name] = spelling, carries
		}
	}
	return nil
}

// toolFlagsTakingAValue names the reviewed flags whose parser reads a value,
// restated here for the reason every table on this seam is: the executor
// imports the catalog, so the catalog cannot ask a tool what it parses. Read
// out of runnerclock.go:91-95, the only dispatched tool with a flag that is
// not a mode switch: --repo is a string, --runs and --hours are ints,
// --selftest and --ci-scan are bools like every flag the other seventeen
// tools parse.
var toolFlagsTakingAValue = map[string][]string{
	"ra8ci:runner-clock": {"repo", "runs", "hours"},
}

// toolFlagTakesAValue reports whether a tool reads the next argument as this
// flag's value, which is what makes the bare spelling swallow an element.
func toolFlagTakesAValue(program, name string) bool {
	for _, candidate := range toolFlagsTakingAValue[program] {
		if candidate == name {
			return true
		}
	}
	return false
}

// ToolFlagTakesAValue reports whether a reviewed tool flag carries a value,
// and is how a caller can tell a flag that can be overwritten from a switch
// that cannot.
func ToolFlagTakesAValue(program, name string) bool {
	return toolFlagTakesAValue(program, name)
}
