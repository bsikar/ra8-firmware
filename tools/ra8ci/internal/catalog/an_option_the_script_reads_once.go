// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The sixth shell door, and the first that judges an argv element against
// another element of the same argv rather than against the script's contract.
//
// Every standing shell door reads one option at a time: whether the parser
// knows the spelling (a_script_option_the_script_parses.go), whether the
// option reports instead of working (a_gate_step_runs_a_gate.go), whether it
// needs a companion beside it (a_container_step_names_a_gate.go), whether the
// mode it selects ever reads it (a_flag_the_mode_reads.go), and whether the
// value after it is one the script accepts
// (a_script_option_value_the_script_accepts.go). Each of those admits
//
//	bash scripts/ci.sh --gate misra --gate format
//
// because each element is, on its own, exactly right.
//
// ci.sh parses with a `while [[ $# -gt 0 ]]` loop over a case, and every arm
// that takes a value assigns to the same variable and shifts. So the second
// --gate overwrites the first and the step runs ONE gate: format. misra is
// dead text in a reviewed definition, and a reader of the catalog has no way
// to tell which of the two the runner honours short of reading the parser.
// Nothing fails, the verdict filed is a real verdict, and it is a verdict for
// work the step only half asked for.
//
// REFUSED: a repeat of an option that CARRIES A VALUE, in any spelling. The
// value written first is the one discarded, so the refusal names both and
// says which one the script keeps.
//
// ADMITTED: a repeat of a valueless switch. --fast --fast selects the same
// mode twice and discards nothing; there is no second intention for the
// parser to drop, so refusing it would be the door judging tidiness rather
// than meaning. Pinned by TestARepeatedSwitchIsNotRefusedHere.
//
// The two spellings of one option are the SAME option here. --gate misra and
// --gate=format set one variable through two case arms, so the name is read
// through the script's own tables and the =form is cut down to its name
// before counting, the way the collision rule in args_collision.go reads a
// reviewed argument for the name it claims.
//
// A script absent from reviewedScriptOptions is admitted on its path alone,
// as it is by every other door here: the rule extends where a contract is
// stated rather than inventing a uniform shell contract no script signed up
// to.
func checkNoScriptOptionIsNamedTwice(step Step) error {
	if len(step.Args) == 0 {
		return nil
	}
	script := step.Args[0]
	stated, known := reviewedScriptOptions[script]
	if !known {
		return nil
	}
	seen := make(map[string]string, len(step.Args))
	rest := step.Args[1:]
	for index := 0; index < len(rest); index++ {
		arg := rest[index]
		name, value, carries := scriptOptionNamed(stated, arg, rest, index)
		if !carries {
			continue
		}
		if statesExactly(stated.takingTheNextArgument, arg) {
			index++
		}
		if first, repeated := seen[name]; repeated {
			return fmt.Errorf("%w: step %q passes %s to %q twice, as %q and then %q; the script assigns one variable for both and keeps the last, so it would run %q and drop %q without saying so",
				ErrInvalidCatalog, step.Name, name, script, first, valueSpelling(arg, value), value, valueOf(first))
		}
		seen[name] = valueSpelling(arg, value)
	}
	return nil
}

// scriptOptionNamed reads one argv element for the option it names and the
// value it carries. It reports carries false for anything that carries no
// value: a valueless switch, and an element the option door already refuses.
func scriptOptionNamed(stated scriptOptions, arg string, rest []string, index int) (name, value string, carries bool) {
	if statesExactly(stated.takingTheNextArgument, arg) {
		if index+1 >= len(rest) {
			return "", "", false
		}
		return arg, rest[index+1], true
	}
	cut, after, split := strings.Cut(arg, "=")
	if !split || !statesExactly(stated.takingAnEqualsValue, cut) {
		return "", "", false
	}
	return cut, after, true
}

// valueSpelling writes an option back the way the step spelled it, so the
// refusal quotes the two elements a reader would go looking for.
func valueSpelling(arg, value string) string {
	if strings.Contains(arg, "=") {
		return arg
	}
	return arg + " " + value
}

// valueOf returns the value inside a spelling valueSpelling produced.
func valueOf(spelling string) string {
	if name, value, split := strings.Cut(spelling, "="); split {
		_ = name
		return value
	}
	_, value, _ := strings.Cut(spelling, " ")
	return value
}

// ScriptOptionCarriesAValue reports whether a script's parser reads a value
// for an option, in either spelling, and is how a caller can tell an option
// that can be overwritten from a switch that cannot.
func ScriptOptionCarriesAValue(script, option string) bool {
	stated, known := reviewedScriptOptions[script]
	if !known {
		return false
	}
	return statesExactly(stated.takingTheNextArgument, option) ||
		statesExactly(stated.takingAnEqualsValue, option)
}
