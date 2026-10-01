// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// The bound mirror of a_target_count_the_tool_takes.go, and it stands in the
// same relation to that door as a_bound_path_beside_a_whole_tree_scan.go does
// to an_all_step_names_the_whole_tree.go.
//
// That door counts the file arguments a step STATES and holds them to the
// ceiling the tool's own parser states: ascii is the only one of the seven
// file-reading tools with a ceiling, exactly one target unless --all. A
// DECLARED POSITIONAL is the same count arriving later. BindArguments spells
// every positional as a bare argv element (args.go), StepArgv appends the
// bound argv AFTER the reviewed arguments, and the executor hands the whole
// line to the program. So a task declaring one positional against a step that
// already names a target for ra8ci:ascii produces flags.NArg() == 2 and the
// usage error the step door exists to prevent, except nothing sees it until a
// caller supplies the value. Two declared positionals do it against a step
// naming no target at all.
//
// NEITHER NEIGHBOUR CATCHES IT. checkBoundArgumentsAreOnesTheToolTakes asks
// whether the tool reads a path AT ALL, and ascii does. checkNoBoundPath
// ContradictsAWholeTreeScan asks whether a bound path contradicts a scope the
// step already named, and a step naming one target named no scan. What is
// wrong here is only the total, which is why it is its own door and why it
// reads the same ceiling table rather than restating one.
//
// DELIBERATELY NOT JUDGED: a declared FLAG. Binding spells it --name=value, an
// option rather than a target, so it changes no count; whether ascii parses an
// option of that name is checkBoundArgumentsAreOnesTheToolTakes's question and
// it is already answered.
//
// This is a TASK rule because only the task states a schema, the same line the
// other two bound doors draw. It is written over every tool step rather than
// over the one step checkArgumentsReachOneStep allows, so the two rules stay
// independent.
func checkBoundTargetsFitTheToolsCeiling(task Task) error {
	if len(task.ArgsSchema.Positional) == 0 {
		return nil
	}
	for _, step := range task.Steps {
		program := step.Program
		ceiling, stated := toolsTakingOneTarget[program]
		if !stated {
			continue
		}
		named := targetsNamed(step.Args, valueTakingToolFlags[program])
		total := len(named) + len(task.ArgsSchema.Positional)
		if total <= ceiling {
			continue
		}
		return fmt.Errorf("%w: task %q declares %d positional argument(s) and step %q already names %d target(s) for %q, which takes %s; binding spells each positional as a bare argument after the reviewed ones, so the tool would answer the dispatch with a usage error as soon as a caller supplied a value",
			ErrInvalidCatalog, task.Name, len(task.ArgsSchema.Positional), step.Name,
			len(named), program, targetWord(ceiling))
	}
	return nil
}
