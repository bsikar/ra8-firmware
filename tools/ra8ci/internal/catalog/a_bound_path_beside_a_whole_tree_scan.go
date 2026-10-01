// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// The bound mirror of checkAnAllStepNamesTheWholeTree. That door refuses a
// reviewed step naming a path beside --all, because --all answers "what should
// I look at" with the whole tree and a path beside it is a contradiction about
// scope that every tool taking the flag refuses: ascii errors on
// *all && flags.NArg() != 0, and assert-casts, final-newline, no-null and
// since read --all only as len(args) == 1 and answer anything longer with a
// usage line. All exit 2 WITHOUT SCANNING.
//
// A DECLARED POSITIONAL is the same contradiction arriving later. Binding
// spells it as a bare argv element and StepArgv appends it after the reviewed
// arguments (args.go), so a task declaring a positional against a step that
// already names --all produces exactly the argv the other door refuses, except
// that nothing sees it until a caller supplies the value. The step was
// admitted by review, digested and shipped, and fails identically on every
// runner from the first dispatch that carries the argument.
//
// It cannot be folded into the reviewed door, because that one judges a STEP
// and the schema is stated by the TASK; and it is not the same rule as
// checkBoundArgumentsAreOnesTheToolTakes, which asks whether the tool reads a
// path AT ALL. These five DO read one. The mistake here is declaring a path
// for a step that already said it wants everything.
//
// Deliberately says nothing about a declared FLAG beside --all: a flag binds
// as --name=value, which is an option rather than a target, and whether the
// pair is legal is the companion question checkAnAllStepNamesTheWholeTree
// already settles against reviewedToolFlags.
func checkNoBoundPathContradictsAWholeTreeScan(task Task) error {
	if len(task.ArgsSchema.Positional) == 0 {
		return nil
	}
	for _, step := range task.Steps {
		if _, named := ToolProgram(step.Program); !named {
			continue
		}
		for _, arg := range step.Args {
			if flagNameArgvClaims(arg) != allFlag {
				continue
			}
			return fmt.Errorf("%w: task %q declares the positional argument %q and step %q already names %q to %q; the bound value would reach the tool as a path beside a whole-tree scan, which it answers with a usage error and no reading of the tree",
				ErrInvalidCatalog, task.Name, task.ArgsSchema.Positional[0], step.Name, arg, step.Program)
		}
	}
	return nil
}
