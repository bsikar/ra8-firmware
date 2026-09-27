// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// The bound mirror of a_tool_flag_the_parser_still_reads.go, and the door that
// picks up exactly what a_bound_target_the_tool_can_read.go set down.
//
// That door counts the targets a task declares and holds the total to the
// tool's ceiling, and says so plainly about the other half: a declared FLAG is
// not judged there, because binding spells it --name=value, an option rather
// than a target, so it changes no count. That is true of the COUNT and false
// of the POSITION, which is what this door is for.
//
// BindArguments (args.go:151-171) writes every declared positional first, as a
// bare value, and every declared flag after them, as --name=value. StepArgv
// appends that whole bound argv AFTER the reviewed arguments. So for the three
// tools that parse with the flag package, a declared flag can only ever arrive
// after a target when one is on the line at all, and the flag package stops
// reading options at the first argument that is not one. The flag is read as
// another target.
//
// TWO WAYS IT HAPPENS, and both are invisible from either end alone:
//   - the reviewed step already names a target (ra8ci:ascii src/a.c) and the
//     task declares a flag, which binds behind it;
//   - the task declares a positional and a flag, and the flag binds behind the
//     positional whatever the step says.
//
// In both, ascii receives two targets and no option: parseOptions answers
// "pass one target path, optionally with --checkout, or pass --all" and exits
// 2 before the tree is read, and the operator reading history sees a failing
// ASCII gate rather than a step that never ran one.
//
// NEITHER NEIGHBOUR CATCHES IT. checkBoundArgumentsAreOnesTheToolTakes asks
// whether the tool parses an option of that name, and ascii parses --checkout.
// checkBoundTargetsFitTheToolsCeiling counts positionals and deliberately
// counts no flags. checkNoToolFlagFollowsATarget reads the reviewed argv,
// which is in the right order. What is wrong is where BINDING puts the flag,
// and only a task states a schema, so only a task rule can see it.
//
// A tool that compares argv element by element finds its flag wherever it
// sits, so the fifteen are not judged here either; the table this reads is the
// one the reviewed door states, so the two halves cannot disagree about which
// parser stops early.
func checkNoBoundFlagLandsBehindATarget(task Task) error {
	if len(task.ArgsSchema.Flags) == 0 {
		return nil
	}
	for _, step := range task.Steps {
		program := step.Program
		if !toolsStoppingAtTheFirstTarget[program] {
			continue
		}
		reviewed := len(targetsNamed(step.Args, valueTakingToolFlags[program]))
		if reviewed == 0 && len(task.ArgsSchema.Positional) == 0 {
			continue
		}
		return fmt.Errorf("%w: task %q declares the flag argument(s) %v and step %q dispatches %q with %d reviewed target(s) and %d declared positional(s); binding writes every flag after every positional and appends them behind the reviewed argv, and that parser stops reading options at the first argument that is not one, so the bound flag would arrive as another target",
			ErrInvalidCatalog, task.Name, task.ArgsSchema.Flags, step.Name, program,
			reviewed, len(task.ArgsSchema.Positional))
	}
	return nil
}
