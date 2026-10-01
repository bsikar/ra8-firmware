// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"fmt"
	"io"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// RunWithArguments executes a reviewed task whose caller supplies named
// argument values, binding them against the task's reviewed schema before any
// process is started.
//
// Run is this function with no values, so both entry points reach a command
// the same way: the reviewed argv from the catalog, then the bound values,
// one argv element each. Nothing is ever concatenated into a string, so the
// binding cannot introduce a shell, a second command, or a redirection.
//
// Values are refused as a whole when any one of them is not declared,
// missing, or outside the reviewed value alphabet; a task never runs with a
// partially bound argument list, because a step that ran with half of what
// was asked for would report the outcome of something nobody requested.
func RunWithArguments(ctx context.Context, root string, task catalog.Task, values map[string]string,
	stdout, stderr io.Writer, stepWriters ...func(string) (io.Writer, io.Writer)) (Result, error) {
	return runReviewed(ctx, root, task, values, stdout, stderr, stepWriters...)
}

// boundStep is the argv one step actually runs. It always rebuilds the
// argument list through catalog.StepArgv, even when nothing is bound, so a
// step is dispatched from a copy rather than from the reviewed definition's
// own backing array and the reviewed arguments are checked on every path.
//
// Bound values land after the reviewed ones. That ordering is what keeps the
// dispatch seam intact: a step is either an ra8ci tool or bash whose first
// argument is a reviewed script path, and an argument appended to the end can
// never take the place of either.
func boundStep(step catalog.Step, bound []string) (catalog.Step, error) {
	argv, err := catalog.StepArgv(step, bound)
	if err != nil {
		return catalog.Step{}, fmt.Errorf("%w: step %s: %v", ErrUnreviewedTask, step.Name, err)
	}
	step.Args = argv
	return step, nil
}
