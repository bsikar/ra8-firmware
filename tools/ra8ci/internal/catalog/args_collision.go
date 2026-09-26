// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// endOfOptions is the marker a program reads as "everything after this is an
// operand", whatever it looks like.
const endOfOptions = "--"

// checkBoundArgumentsKeepTheirMeaning refuses a reviewed task whose own step
// argv would change what a bound argument means.
//
// StepArgv states the rule this enforces: the reviewed arguments come first
// because they are what a reader of the catalog can see, and a bound value
// must not be able to change what they mean. That is stated as an ordering,
// and ordering alone does not carry it. Two reviewed shapes defeat it.
//
// A reviewed argument that already carries a declared flag name. Binding
// appends --name=value after it, so the program is handed the same flag twice
// and which one wins is the flag package's business, not the catalog's. Go's
// own flag package takes the last, which means a caller's value silently
// replaces a value review pinned. The reviewed definition still reads as
// though the pinned one applies.
//
// A reviewed "--" among the step's arguments. Everything a binding appends
// lands after the end-of-options marker, so a declared flag arrives as an
// operand rather than a flag: --mode=fast becomes a file name. The task does
// not fail, it runs with the argument dropped and an extra operand nobody
// asked for.
//
// Both are admission rules, applied where a manifest is read rather than
// against a task already persisted under a reviewed digest, the same line
// ValidateTask draws against the dispatch seam. Neither refuses anything in
// the v1 catalog: one task declares a schema (ascii-rewrite, one positional,
// no flags) and no reviewed step anywhere passes "--".
func checkBoundArgumentsKeepTheirMeaning(task Task) error {
	if len(task.ArgsSchema.Positional) == 0 && len(task.ArgsSchema.Flags) == 0 {
		return nil
	}
	declared := make(map[string]bool, len(task.ArgsSchema.Flags))
	for _, name := range task.ArgsSchema.Flags {
		declared[name] = true
	}
	for _, step := range task.Steps {
		for _, arg := range step.Args {
			if arg == endOfOptions {
				return fmt.Errorf("%w: task %q declares arguments and its step %q passes %q, which would make every bound argument an operand",
					ErrInvalidCatalog, task.Name, step.Name, endOfOptions)
			}
			if !strings.HasPrefix(arg, "--") {
				continue
			}
			name, _, _ := strings.Cut(strings.TrimPrefix(arg, "--"), "=")
			if declared[name] {
				return fmt.Errorf("%w: task %q declares flag %q and its step %q already passes %q; a bound value would be a second %s the program reads instead",
					ErrInvalidCatalog, task.Name, name, step.Name, arg, arg[:2]+name)
			}
		}
	}
	return nil
}
