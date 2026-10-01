// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// A reviewed task states two command lines, not one. The step states the argv
// a reader of the catalog can see, and the ARGS SCHEMA states the argv a
// caller may add later: BindArguments spells every declared positional as a
// bare value and every declared flag as --name=value (args.go), StepArgv
// appends them after the reviewed arguments, and the executor hands the whole
// thing to the program.
//
// Six doors already judge the reviewed half against the tool that receives it
// (the program name, the option names, the exclusive self test, which tools
// read a file argument, --all naming no path, and the values runner-clock
// accepts). Nothing judged the OTHER half. A task could declare an argument
// the dispatched tool has no way to read, and the definition was admitted,
// digested and shipped; the mistake only appeared once somebody supplied the
// argument, at which point the bound element reached a tool that answers what
// it does not recognise with a usage line and exit 2. Once per dispatch, on
// every runner, and the gate result an operator reads says the repository
// violated the standard rather than that the definition was wrong.
//
// TWO REFUSALS, both of which hold whatever value the caller supplies:
//
//   - A DECLARED POSITIONAL binds as a bare argv element. Eleven of the
//     eighteen tools read no file arguments at all (a_file_argument_the_tool
//     _reads.go) and answer argv past their options with usage, so a bare
//     value is refused there for the same reason a reviewed path is.
//   - A DECLARED FLAG binds as --name=value. A tool that does not parse an
//     option of that name refuses it: the flag-package tools with "flag
//     provided but not defined", the fifteen comparing argv exactly with a
//     usage line. Both exit 2.
//
// DELIBERATELY NOT JUDGED: a declared flag the tool DOES parse. Whether
// --name=value is then readable depends on the value, and the value arrives at
// dispatch, long after review. runner-clock's --hours=3 is exactly how that
// option is meant to be supplied; ascii's --all=true parses as a boolean and
// --all=yes does not, and review cannot pin which a caller will send. That is
// the value half, which checkToolFlagValuesAreOnesTheToolAccepts judges for
// the reviewed argv and no admission rule can judge for the bound one.
//
// wave-references is the reason this is about more than exit codes: its Run
// reads argv only for --selftest and silently ignores everything else, so a
// bound argument it cannot use does not fail at all. The step runs green and
// files a verdict for a scope nobody declared.
//
// This is a TASK rule, not a step rule, because only the task states a schema.
// checkArgumentsReachOneStep already holds a task that declares arguments to a
// single step, so in practice this judges that step; it is written over every
// tool step so the two rules stay independent.
//
// Restated, not imported, for the same reason every table on this seam is: the
// executor imports the catalog, so the catalog cannot ask the executor what
// its tools parse. Both tables it reads are the ones the earlier doors already
// pin to the dispatch list.
func checkBoundArgumentsAreOnesTheToolTakes(task Task) error {
	if len(task.ArgsSchema.Positional) == 0 && len(task.ArgsSchema.Flags) == 0 {
		return nil
	}
	for _, step := range task.Steps {
		program := step.Program
		if _, named := ToolProgram(program); !named {
			continue
		}
		accepted, known := reviewedToolFlags[program]
		if !known {
			continue
		}
		if len(task.ArgsSchema.Positional) != 0 && !toolsReadingFileArguments[program] {
			return fmt.Errorf("%w: task %q declares the positional argument %q and step %q dispatches %q, which reads no file arguments; the bound value would reach it as argv past its options and every runner would answer with a usage error; only %s take a path",
				ErrInvalidCatalog, task.Name, task.ArgsSchema.Positional[0], step.Name, program,
				strings.Join(toolsThatReadFileArguments(), ", "))
		}
		for _, name := range task.ArgsSchema.Flags {
			if !statesFlag(accepted, name) {
				return fmt.Errorf("%w: task %q declares the flag %q and step %q dispatches %q, which parses only %s; binding spells it %s and the tool would answer it with a usage error",
					ErrInvalidCatalog, task.Name, name, step.Name, program,
					strings.Join(dashed(accepted), ", "), "--"+name+"=value")
			}
		}
	}
	return nil
}
