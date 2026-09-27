// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The bound half of the SHELL dispatch, and the last quarter of this seam that
// nothing judged.
//
// A reviewed task states two command lines. The step states the argv a reader
// of the catalog can see; the ARGS SCHEMA states the argv a caller may add
// later. BindArguments spells every declared positional as a bare value and
// every declared flag as --name=value (args.go:152-171), StepArgv appends the
// bound argv AFTER the reviewed one, and the executor hands the whole thing to
// the program.
//
// Both halves of the tool shape are judged: the reviewed argv by six doors,
// and what binding adds by three more (whether the tool takes the argument at
// all, whether a bound path contradicts a scan, whether the targets still fit
// the tool's ceiling). The shell shape has only its REVIEWED half judged, by
// the option door and the reporting-mode door. Every one of the three bound
// doors opens with `if _, named := ToolProgram(program); !named { continue }`,
// so a task dispatching a script walked past all of them.
//
// So a task could declare an argument scripts/ci.sh has no way to read. The
// definition was admitted, digested and shipped, and the mistake appeared only
// once somebody supplied the argument, on a runner, in a guest, past the point
// where anyone was reading a catalog.
//
// TWO REFUSALS, both of which hold whatever value the caller supplies, because
// what is wrong is the SPELLING binding produces, not the value inside it:
//
//   - A DECLARED POSITIONAL binds as a bare argv element. ci.sh's parser is a
//     case over option spellings with no positional arm at all: a bare value
//     falls to `*)`, which prints "ci.sh: unknown flag" and exits 2 before a
//     gate runs. No stated script takes a positional, so this is refused
//     wherever a contract is known.
//   - A DECLARED FLAG binds as --name=value, and THE =FORM IS A DIFFERENT
//     SPELLING FROM THE FLAG. A shell case arm matches exactly. ci.sh has one
//     --gate=* arm and nothing else, so --fast is parsed and --fast=1 is not.
//
// THE SECOND REFUSAL IS THE ONE THIS DOOR EXISTS FOR, and it is invisible from
// either end on its own. A reviewer reading `"flags": ["fast"]` checks ci.sh,
// finds --fast in the parser, and is right. A reviewer reading the step finds
// nothing wrong with it either, because the step does not name --fast; binding
// does, later, in a spelling neither of them ever sees written down. The
// refusal names that spelling, since it is the whole mistake.
//
// The option door (a_script_option_the_script_parses.go) cannot make this
// call: it judges step.Args, and a declared flag is never in step.Args. This
// door judges the task, like its three tool-side neighbours, and reads the
// same reviewedScriptOptions table so the reviewed and bound halves of the
// shell shape agree on what each script parses.
//
// DELIBERATELY NOT JUDGED: the value a caller sends for a flag the script does
// parse with an =arm. --gate=format is exactly how that option is meant to be
// supplied; whether the gate name exists is the registry's question and the
// value arrives at dispatch, long after review. That is the same line
// checkBoundArgumentsAreOnesTheToolTakes draws on the tool side.
//
// A script absent from reviewedScriptOptions is admitted on its path alone, as
// it is by the option door: the rule extends where a contract is stated rather
// than inventing a uniform shell contract no script signed up to.
func checkBoundArgumentsAreOnesTheScriptParses(task Task) error {
	if len(task.ArgsSchema.Positional) == 0 && len(task.ArgsSchema.Flags) == 0 {
		return nil
	}
	for _, step := range task.Steps {
		if step.Program != DispatchShell || len(step.Args) == 0 {
			continue
		}
		script := step.Args[0]
		stated, known := reviewedScriptOptions[script]
		if !known {
			continue
		}
		if len(task.ArgsSchema.Positional) != 0 {
			return fmt.Errorf("%w: task %q declares the positional argument %q and step %q dispatches %q, which parses only options (%s); binding spells a positional as a bare argument and the script would answer it with an unknown-flag error before any gate ran",
				ErrInvalidCatalog, task.Name, task.ArgsSchema.Positional[0], step.Name, script,
				strings.Join(scriptSpellings(stated), ", "))
		}
		for _, name := range task.ArgsSchema.Flags {
			if err := checkBoundFlagSpelling(task, step, script, stated, name); err != nil {
				return err
			}
		}
	}
	return nil
}

// checkBoundFlagSpelling judges one declared flag against the script that
// would receive it, in the spelling binding gives it. It separates the two
// refusals because they are different mistakes: a name the script never heard
// of, and a name it knows in a form binding cannot produce.
func checkBoundFlagSpelling(task Task, step Step, script string, stated scriptOptions, name string) error {
	bound := "--" + name
	if statesExactly(stated.takingAnEqualsValue, bound) {
		return nil
	}
	if statesExactly(stated.valueless, bound) || statesExactly(stated.takingTheNextArgument, bound) {
		return fmt.Errorf("%w: task %q declares the flag %q and step %q dispatches %q, which parses %s but has no %s=value arm; binding spells it %s=value and the script would answer that spelling with an unknown-flag error",
			ErrInvalidCatalog, task.Name, name, step.Name, script, bound, bound, bound)
	}
	return fmt.Errorf("%w: task %q declares the flag %q and step %q dispatches %q, which parses only %s; binding spells it %s=value and the script would answer it with an unknown-flag error before any gate ran",
		ErrInvalidCatalog, task.Name, name, step.Name, script,
		strings.Join(scriptSpellings(stated), ", "), bound)
}

// ScriptTakesABoundFlag reports whether a declared flag reaches a stated
// script in a spelling its parser accepts, and is how a caller can tell a flag
// the script takes from one that only looks like it.
func ScriptTakesABoundFlag(script, name string) bool {
	stated, known := reviewedScriptOptions[script]
	if !known {
		return false
	}
	return statesExactly(stated.takingAnEqualsValue, "--"+name)
}
