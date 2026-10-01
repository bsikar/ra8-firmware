// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// checkRestoreProbeCoversTheFlashRestore refuses a reviewed HIL task whose
// declared restore-and-probe bound is shorter than the flash restore the same
// definition budgets for.
//
// Both numbers are reviewed, both sit in the same HIL block, and they are
// validated apart and never against each other: ValidateHILTaskMetadata
// range-checks FlashRestoreSeconds at 1..3600, validateHandoffBounds
// range-checks HandoffRestoreProbeSeconds at 1..maxHandoffBoundSeconds and
// then compares only the SAFE STEP against the safety maximum. A task
// declaring a 60s flash restore and an 8s restore-and-probe passes both.
//
// They describe overlapping work in one direction. FlashRestoreSeconds is
// what putting the board's flash back is expected to cost, and it is spent on
// every attempt: store.attempts holds the board lease open for the attempt
// deadline PLUS FlashRestoreSeconds, boardagent's timing decision carries it
// as Decision.FlashRestoreBound, and both boardclient.validHILTimingAssignment
// and store.validateHILTimingEvidence refuse a decision whose bound is not
// exactly that number. HandoffRestoreProbeSeconds is the longest restore AND
// probe that follows the interrupted step before the board is neutral again,
// so the restore sits inside it. A probe bound below it claims the whole of
// that work finishes sooner than its own restore alone.
//
// The cost lands where the declared numbers are believed rather than
// measured. HandoffSafetyBound is the sum of the two, board.EstimateHandoff
// starts every estimate at it and may only ever raise a quoted target above
// it (the estimator lengthens a handoff, never shortens the bound the task
// declared), and the answer is served to a waiter as safety_bound_seconds.
// With a probe bound under the flash restore, the floor under "when do I get
// the board back" sits below what this definition already says the restore
// costs, and automatic dispatch is held off for only that shorter window.
//
// The safe step is left alone here: it is the work the restore interrupts,
// not work the restore contains, and it already has two rules of its own,
// validateHandoffBounds against the safety maximum and
// checkSafeStepFitsTheDeadline against the task deadline. Undeclared bounds
// stay a real answer, the line HandoffBoundsDeclared and validateHandoffBounds
// already draw.
//
// This is an admission rule, applied where a manifest is read, not a re-check
// of a task already persisted against a reviewed digest. It reads only HIL
// fields and so could sit in ValidateHILTaskMetadata, but that function is
// re-applied to held definitions by store.attempts and store.HeldYieldWork,
// where a new refusal would retire work already running. A definition
// admitted under an older rule keeps running, the same line its two sibling
// rules take.
func checkRestoreProbeCoversTheFlashRestore(task Task) error {
	if task.HIL == nil || !task.HIL.HandoffBoundsDeclared() {
		return nil
	}
	if task.HIL.HandoffRestoreProbeSeconds < task.HIL.FlashRestoreSeconds {
		return fmt.Errorf("%w: HIL task %q declares a %ds restore-and-probe over a %ds flash restore; the handoff ETA is floored below the restore this task always pays",
			ErrInvalidCatalog, task.Name, task.HIL.HandoffRestoreProbeSeconds, task.HIL.FlashRestoreSeconds)
	}
	return nil
}
