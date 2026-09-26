// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// checkStepOutcomesAgreeWithTheAttempt holds a receipt's per-step verdicts to
// the one it states for the attempt as a whole. The two are not independent
// readings: the executor copies the running step's exit code, timeout and
// cancellation onto the result and stops there (executor.go, runTask assigning
// result.ExitCode/TimedOut/Cancelled from the step it just ran and returning as
// soon as any of them reports trouble), and the agent then derives the outcome
// from that same result (agent.go, terminalReceipt choosing timed_out,
// cancelled, failed or succeeded). So a step's verdict and the attempt's are
// two views of one event.
//
// Validate held each half alone. The outcome switch judged Outcome against
// ChildExitCode, TimedOut and Cancelled, and the per-step loop judged a step's
// stamps and byte counts, and nothing compared one with the other. A receipt
// could state that the attempt succeeded while carrying a step that exited
// non-zero, or claim it neither timed out nor was cancelled while carrying a
// step reporting that it did.
//
// Which half a reader trusts decides what the attempt means. The outcome is
// what the dispatcher records and what a retry policy reads; the step verdicts
// are what an operator opens to find out which step went wrong. A receipt whose
// two halves disagree tells those two readers different stories about the same
// attempt, and nothing downstream can tell which one was the mistake.
//
// The rule refuses the contradictions and nothing else. It does not require a
// failed attempt to carry a failing step, because an attempt can fail before or
// between its steps (a bind error, a log upload error, a cancellation the
// executor takes at the task level), and the receipt says so honestly with
// every step it did run reporting clean.
func checkStepOutcomesAgreeWithTheAttempt(receipt TerminalReceipt) error {
	for _, step := range receipt.Steps {
		if step.TimedOut && !receipt.TimedOut {
			return fmt.Errorf("%w: step %s reports a timeout the attempt does not", ErrInvalid, step.Name)
		}
		if step.Cancelled && !receipt.Cancelled {
			return fmt.Errorf("%w: step %s reports a cancellation the attempt does not", ErrInvalid, step.Name)
		}
		if receipt.Outcome == "succeeded" && step.ExitCode != 0 {
			return fmt.Errorf("%w: attempt succeeded with step %s exiting %d", ErrInvalid, step.Name, step.ExitCode)
		}
	}
	return nil
}
