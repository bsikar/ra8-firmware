// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// checkSafeStepCoversTheObservation refuses a reviewed HIL task whose longest
// indivisible step is shorter than the observation the same definition says
// it may spend in one step.
//
// Both numbers are reviewed, both sit in the same HIL block, and they are
// validated apart and never against each other. ValidateHILTaskMetadata
// range-checks TimeoutSeconds at 1..3600; validateHandoffBounds range-checks
// HandoffSafeStepSeconds at 1..maxHandoffBoundSeconds and then compares it
// only against SafetyMaximumSeconds, which a task may leave undeclared and
// which binds in the other direction, as a cap rather than a floor. A task
// declaring a 300s observation and a 20s indivisible step passes both.
//
// They describe the same seconds in one direction. The observation is one
// STEP: validateHILTask refuses a HIL block whose ObservationStep names no
// step of the task, and boardagent's HIL runner gives exactly that step its
// own bound, decision.ValidityWindow, where every other step runs under the
// task deadline (boardagent.hil_execute). That window is never below the
// declared timeout: hilspec.Decide takes the declared number as its fallback
// and hilpolicy.Choose raises its estimate to the fallback rather than below
// it. So a declared timeout is a lower bound on how long this task's
// observation step may be in flight, and the safe step is the declared
// longest step it may be in the middle of when it is asked to yield. A safe
// step under the timeout claims the task can always be interrupted sooner
// than its own longest step is allowed to run.
//
// The cost lands where the declared number is believed rather than measured.
// HandoffSafetyBound is the sum of the two handoff bounds,
// board.EstimateHandoff starts every estimate at it and may only ever raise a
// quoted target above it, and the answer is served to a waiter as
// safety_bound_seconds. With a safe step under the observation timeout, the
// floor under "when do I get the board back" sits below the one step this
// definition already says may still be running, so a waiter is quoted a
// target the observation alone can outlast, and automatic dispatch is held
// off for only that shorter window.
//
// Only a DECLARED timeout is judged, and only against DECLARED bounds. A task
// that declares no timeout is not held to hilpolicy.DefaultSeconds here: that
// 30s is the policy layer's choice for a definition that stated nothing, not
// a number the definition claims, the same line checkObservationTimeout-
// FitsTheDeadline draws. Undeclared handoff bounds stay a real answer, the
// line HandoffBoundsDeclared and validateHandoffBounds already draw.
//
// This is an admission rule, applied where a manifest is read, not a re-check
// of a task already persisted against a reviewed digest. It reads only HIL
// fields and so could sit in ValidateHILTaskMetadata, but that function is
// re-applied to held definitions by store.attempts and store.HeldYieldWork,
// where a new cross-field refusal would retire work already running. A
// definition admitted under an older rule keeps running, the same line its
// three sibling rules take.
func checkSafeStepCoversTheObservation(task Task) error {
	if task.HIL == nil || !task.HIL.HandoffBoundsDeclared() || !task.HIL.TimeoutDeclared {
		return nil
	}
	if task.HIL.HandoffSafeStepSeconds < task.HIL.TimeoutSeconds {
		return fmt.Errorf("%w: HIL task %q declares a %ds indivisible step under a %ds observation timeout; the handoff ETA is floored below the one step that may still be running",
			ErrInvalidCatalog, task.Name, task.HIL.HandoffSafeStepSeconds, task.HIL.TimeoutSeconds)
	}
	return nil
}
