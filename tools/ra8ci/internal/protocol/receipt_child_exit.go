// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// checkChildExitIsTheLastStepsExit holds a receipt's attempt-level child exit
// code to the step that produced it. The two are not independent readings. The
// executor copies each step's exit code onto the attempt as it runs and returns
// as soon as a step reports trouble (executor.go, runBoundTask assigning
// result.ExitCode = stepResult.ExitCode inside the loop), so the attempt's exit
// code is always the exit code of the last step it reports. The agent then
// states that number and only that number: terminalReceipt sets ChildExitCode
// from result.ExitCode when the task started and the code is not negative
// (agent.go), and leaves it absent otherwise.
//
// Both ways the code can go absent are already honest. A task that ended
// between two steps drops the attempt code back to noChildExit rather than
// keeping the previous step's clean 0 (executor/between_steps.go), and a step
// whose child never exited carries -1, which the agent does not state. So a
// receipt that carries a child exit code beside at least one step is claiming
// the last of those steps exited with exactly that code.
//
// Nothing read the two together. The outcome switch judges ChildExitCode only
// under "succeeded", where it must be 0, and checkStepOutcomesAgreeWithTheAttempt
// judges a step's exit code against the attempt's OUTCOME rather than against
// its code, so every non-succeeded receipt could state any child exit code it
// liked beside steps reporting another. A failed attempt claiming the child
// exited 1 while its last step reports 0, or claiming 0 while its last step
// reports 2, passed every check in the tree.
//
// The mismatch is read, not ignored. The plane files the code as the attempt's
// child_exit_code (store/dispatch.go) and the offline path files the same
// number into local_runs, and that column is what a later reader quotes for
// "what did it exit with", while the step rows are what the same reader opens
// to find WHICH step exited that way. A receipt whose two halves state
// different numbers answers one question with the other's answer, and no reader
// downstream can tell which half was the mistake.
//
// The rule fires only where both halves exist. A receipt stating no code is
// reporting an attempt no child decided, which is the ordinary shape for a
// timeout, a cancellation, or a failure between steps; a receipt carrying no
// steps has no step to attribute a code to, and checkErrorCodeMatchesTheReport
// already holds the no_step_executed case on its own terms.
func checkChildExitIsTheLastStepsExit(receipt TerminalReceipt) error {
	if receipt.ChildExitCode == nil || len(receipt.Steps) == 0 {
		return nil
	}
	last := receipt.Steps[len(receipt.Steps)-1]
	if *receipt.ChildExitCode != last.ExitCode {
		return fmt.Errorf("%w: attempt reports child exit %d and its last step %s exited %d",
			ErrInvalid, *receipt.ChildExitCode, last.Name, last.ExitCode)
	}
	return nil
}
