// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"errors"
)

// A step is dispatched one of two ways. A step naming an external program
// goes through runCommand, whose first act is to ask whether the attempt's
// context is already spent and, if it is, to report noChildExit with the
// ending it found rather than start anything. A step naming a built-in gate
// (ra8ci:ascii and the rest) is called directly from runStep, and that door
// had no such question: the gate was entered whatever the context said, and
// only afterwards was the expiry noticed and written onto the step.
//
// The loop in runBoundTask checks the context before each step, so the window
// is the gap between that check and the gate call, which is exactly the
// window runCommand closes for the other kind of step. It is small and it is
// real: a deadline or a cancellation that lands in it starts a full scan of
// the checkout that the attempt has no time left to use.
//
// What is recorded is worse than the wasted scan. The gate's return value is
// the step's exit code, and a gate returns a non-zero code to mean the
// repository violates the standard it enforces. So a step that should never
// have started could report a gate failure, and that number is what
// agent.terminalReceipt carries into the attempt row and what an operator
// reads as the reason the attempt ended. The same ending on an external step
// carries noChildExit, which is the honest answer in both cases: nothing
// judged anything, so there is no verdict to report.
//
// Only the front door is shared. An expiry that lands while a gate is
// running is left exactly as it was: the gate ran to the end and its answer
// is a real reading of the tree, which is also what the external path does
// with a child that exits under the wire (runCommand keeps the child's exit
// code and marks the attempt timed out beside it).
func gateRefusedBeforeStart(ctx context.Context) (commandResult, bool) {
	cause := contextExpiration(ctx)
	if cause == nil {
		return commandResult{}, false
	}
	refusal := commandResult{ExitCode: noChildExit}
	refusal.TimedOut = errors.Is(cause, context.DeadlineExceeded)
	refusal.Cancelled = !refusal.TimedOut
	return refusal, true
}
