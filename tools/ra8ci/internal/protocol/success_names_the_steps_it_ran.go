// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// checkSuccessNamesTheStepsItRan holds a terminal receipt's two best claims,
// that the attempt succeeded and that its evidence is complete, to the steps it
// actually reports. Neither claim is free-standing. The agent derives the
// outcome from the executor's result and then downgrades a success carrying no
// steps to a failure outright (agent.go, terminalReceipt setting outcome
// "failed" when result.Steps is empty), and it sets EvidenceComplete only when
// the three error paths are clear AND at least one step ran (agent.go, the
// EvidenceComplete expression ending in len(result.Steps) > 0). So no agent in
// this tree authors either shape with an empty step list.
//
// Validate never said so. The outcome switch judges "succeeded" against
// ChildExitCode, EvidenceComplete, TimedOut and Cancelled, all attempt-level
// fields an empty receipt can satisfy, and every step rule in the tree is a
// loop over Steps, which does nothing at all when there are none:
// checkChildExitIsTheLastStepsExit returns early on an empty list by design,
// checkLogSequenceCoversSteps needs zero chunks for zero bytes, and
// checkErrorCodeMatchesTheReport only judges a step count beside an error code
// the receipt need not state. A receipt reporting a succeeded attempt with
// complete evidence, a child exit of 0 and no steps whatsoever therefore passed
// the whole tree while describing an attempt that ran nothing.
//
// That is the most consequential shape a receipt can carry wrongly. The plane
// writes the outcome into the attempt and reads EvidenceComplete to decide
// whether a green result can be trusted at all (store/dispatch.go,
// taskResultFor downgrading an unverifiable green on that flag), so a receipt
// with neither a failure nor a step is a task marked done, verified, with no
// record of anything having been run. Every other refusal in this file family
// catches a receipt whose halves disagree; this one catches a receipt with
// nothing in the half that carries the work.
//
// The rule refuses those two shapes and nothing else. A failed, timed-out or
// cancelled attempt with no steps is ordinary and honest, because an attempt
// can end before its first step ever starts, and no_step_executed is the code
// the agent states for exactly that; checkErrorCodeMatchesTheReport already
// holds that code to an empty step list from the other direction.
func checkSuccessNamesTheStepsItRan(receipt TerminalReceipt) error {
	if len(receipt.Steps) > 0 {
		return nil
	}
	if receipt.Outcome == "succeeded" {
		return fmt.Errorf("%w: receipt reports a succeeded attempt with no step that ran", ErrInvalid)
	}
	if receipt.EvidenceComplete {
		return fmt.Errorf("%w: receipt states complete evidence with no step that ran", ErrInvalid)
	}
	return nil
}
