// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// checkArgumentsReachOneStep refuses a reviewed task that declares arguments
// and runs more than one step.
//
// Binding produces ONE argv for the task, and the executor appends it to every
// step (executor.boundStep, called per step in runBoundTask). That is right for
// a task whose single step is the work, and silently wrong for a task with
// several: a path bound for the rewrite step would also land on a selftest step
// that never declared it, and the step would run with an argument nobody asked
// it to take. Nothing downstream can tell the difference, because by then the
// value is an ordinary argv element the reviewed definition appears to carry.
//
// The catalog has no way to say WHICH step an argument is for, so the honest
// rule while that is true is that a task taking arguments declares one step.
// Giving arguments a step name is the larger change this defers, and it is a
// catalog schema decision rather than a refusal a runtime can make for itself.
//
// This is an admission rule, applied where a manifest is read, not a re-check
// of a task already persisted against a reviewed digest: a definition admitted
// under an older rule keeps running, which is the same line ValidateTask draws
// against the dispatch seam.
func checkArgumentsReachOneStep(task Task) error {
	declared := len(task.ArgsSchema.Positional) + len(task.ArgsSchema.Flags)
	if declared == 0 || len(task.Steps) == 1 {
		return nil
	}
	return fmt.Errorf("%w: task %q declares %d argument(s) and %d step(s); bound arguments are appended to every step, so a task that takes arguments declares exactly one",
		ErrInvalidCatalog, task.Name, declared, len(task.Steps))
}
