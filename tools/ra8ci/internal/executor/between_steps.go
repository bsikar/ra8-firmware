// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"errors"
)

// noChildExit is the attempt-level exit code for a task no child decided. It
// is the value Result carries from the moment it is built, and the value
// runCommand returns when the context was already spent before the child was
// started: in both cases nothing exited, so there is no exit code to report.
const noChildExit = -1

// endedBetweenSteps is the result of a task whose deadline or cancellation
// landed in the gap between two steps.
//
// The loop in runBoundTask copies the running step's exit code, timeout and
// cancellation onto the result and returns as soon as any of them reports
// trouble, so a step's verdict and the attempt's are two views of one event
// (the rule protocol.checkStepOutcomesAgreeWithTheAttempt holds a receipt to).
// The between-steps arm has no step to copy from. It wrote TimedOut or
// Cancelled and left ExitCode alone, and ExitCode at that point is 0, because
// 0 from the previous step is the only thing that lets the loop reach the next
// one at all. The attempt then reported that it timed out and that its child
// exited cleanly, when the step the verdict is about never started.
//
// The cost is downstream, not in the executor. agent.terminalReceipt sets
// ChildExitCode from any ExitCode >= 0, so a timed_out receipt carried
// child_exit_code 0 into the attempt row, and the offline path does the same
// through server/offline.go into local_runs. Both readers see the exit code of
// a step that succeeded beside a verdict about a step that did not run, and
// neither can tell which half is about which step. protocol.Validate does not
// catch it: the timed_out and cancelled arms judge TimedOut and Cancelled and
// say nothing about ChildExitCode, deliberately, because an attempt can end
// without a child exit and has to be able to say so.
//
// Saying so is what this does. The steps that did run keep their own verdicts
// untouched, including the clean 0 of the last one; only the attempt-level
// code drops back to noChildExit, and ChildExitCode is then absent rather than
// wrong. A cause of nil is not an ending and changes nothing, so the helper
// cannot manufacture a cancellation the context never reported.
func endedBetweenSteps(result Result, cause error) Result {
	if cause == nil {
		return result
	}
	result.TimedOut = errors.Is(cause, context.DeadlineExceeded)
	result.Cancelled = !result.TimedOut
	result.ExitCode = noChildExit
	return result
}
