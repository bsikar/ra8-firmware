// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"fmt"
	"time"
)

// checkStepTimeline holds a terminal receipt's steps to the one timeline they
// were run on. The executor runs a task's steps strictly in sequence and stamps
// each one as it goes (executor.go, runTask calling runStep in order), so the
// steps of a real attempt never overlap and never sit outside the attempt that
// contains them. A receipt is client JSON, though, and nothing before this read
// the steps as a sequence at all: Validate judged each step alone, and the store
// judged a step's duration against the receipt's total, so a receipt could state
// a second step that began before the first one ended, or a step that ran
// entirely outside the window its own attempt claims, and every check in the
// tree passed it.
//
// Those stamps are not decoration. They are what a later reader uses to say
// which step was running when a board went quiet, to line a log chunk up with
// the step that wrote it, and to attribute the attempt's elapsed time between
// its steps. A receipt whose steps overlap or escape their attempt cannot answer
// any of those questions, and answering them wrongly is worse than refusing.
//
// The board path already states this same sequencing rule where it judges HIL
// step evidence (store/board_hil_completion.go, refusing a step that starts
// before the previous one ended), so the agent path states the rule the board
// path already states rather than inventing a second one. The tolerance for the
// containment rule is clockDisagreement, the allowance receipt_duration.go
// already fixes at five seconds for the same reason: the attempt's stamps and a
// step's stamps come from separate readings of the same wall clock.
func checkStepTimeline(receipt TerminalReceipt) error {
	var previousName string
	var previousEnd time.Time
	for _, step := range receipt.Steps {
		if !previousEnd.IsZero() && step.StartedAt.Before(previousEnd) {
			return fmt.Errorf("%w: step %s starts before step %s ended", ErrInvalid, step.Name, previousName)
		}
		if err := checkStepIsWithinAttempt(step, receipt); err != nil {
			return err
		}
		previousName, previousEnd = step.Name, step.EndedAt
	}
	return nil
}

// checkStepIsWithinAttempt refuses a step that ran outside the attempt reporting
// it. Callers reach this only after Validate has held both pairs of stamps in
// order, so a step is outside its attempt exactly when it starts too far before
// the attempt started or ends too far after the attempt ended.
func checkStepIsWithinAttempt(step StepSummary, receipt TerminalReceipt) error {
	if step.StartedAt.Before(receipt.StartedAt.Add(-clockDisagreement)) {
		return fmt.Errorf("%w: step %s starts before the attempt reporting it", ErrInvalid, step.Name)
	}
	if step.EndedAt.After(receipt.EndedAt.Add(clockDisagreement)) {
		return fmt.Errorf("%w: step %s ends after the attempt reporting it", ErrInvalid, step.Name)
	}
	return nil
}
