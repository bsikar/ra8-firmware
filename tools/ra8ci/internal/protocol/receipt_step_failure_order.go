// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// checkAFailingStepEndsTheAttempt holds a receipt's steps to the order the
// executor can actually produce. A task's steps are run strictly in sequence,
// and the loop stops at the first one that reports trouble: runBoundTask
// appends the step result, copies its exit code, timeout and cancellation onto
// the attempt, and returns immediately when the step exited non-zero, timed out
// or was cancelled (executor.go). So a real attempt's failing step is always
// its last step, and every step before it exited zero, in time, uncancelled.
//
// Nothing read the steps that way. checkStepOutcomesAgreeWithTheAttempt holds
// each step's verdict against the attempt's, which catches a step failing under
// an attempt claiming success, and checkStepTimeline holds the stamps in
// sequence, which catches steps that overlap. Between them a receipt could
// still state a failed attempt whose first step exited non-zero and whose
// second step then ran and exited clean, and every check in the tree passed it.
//
// That shape is not a smaller version of the truth, it is a different story
// about the run. An operator opening a failed attempt looks for the step that
// went wrong and reads the ones after it as work that happened anyway; a retry
// policy reading a mid-list failure has to decide whether the later steps'
// evidence still counts. Neither question has an answer here, because the
// executor never ran the later steps at all. This refuses the shape rather than
// leaving each reader to guess which half of the receipt to believe.
//
// It states only what the executor guarantees. A failure is allowed in the last
// position and nowhere else, and an attempt whose steps all report clean is
// left alone: an attempt can fail before or between its steps, and the receipt
// says so honestly with clean steps, which checkStepOutcomesAgreeWithTheAttempt
// already pins.
func checkAFailingStepEndsTheAttempt(receipt TerminalReceipt) error {
	if len(receipt.Steps) < 2 {
		return nil
	}
	for _, step := range receipt.Steps[:len(receipt.Steps)-1] {
		if step.TimedOut {
			return fmt.Errorf("%w: step %s timed out with steps reported after it", ErrInvalid, step.Name)
		}
		if step.Cancelled {
			return fmt.Errorf("%w: step %s was cancelled with steps reported after it", ErrInvalid, step.Name)
		}
		if step.ExitCode != 0 {
			return fmt.Errorf("%w: step %s exited %d with steps reported after it", ErrInvalid, step.Name, step.ExitCode)
		}
	}
	return nil
}
