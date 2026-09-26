// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// checkObservationTimeoutFitsTheDeadline refuses a reviewed HIL task whose
// declared observation timeout is longer than the deadline its own attempt
// runs under.
//
// The two numbers are validated apart and never against each other.
// ValidateHILTaskMetadata range-checks TimeoutSeconds at 1..3600 and is handed
// only the HIL block, so the cap the attempt actually runs under,
// DeadlineSeconds on the task, is outside what it can see; ValidateTask
// range-checks DeadlineSeconds at 1..86400 and says nothing about the HIL
// block. A task declaring a ten-minute observation inside a one-minute
// deadline passes both.
//
// Such a task cannot be dispatched at all. The declared timeout is the
// fallback hilspec.Decide hands hilpolicy.Choose, and Choose raises its
// estimate to the fallback rather than below it (bound = max(bound,
// fallback)), so the resulting Decision.ValidityWindow is at least the
// declared timeout. Both ends then refuse a window wider than the deadline:
// boardclient.validHILTimingAssignment and store.validateHILTimingEvidence
// each have decision.ValidityWindow > taskDeadlineSeconds among their
// refusals. So the timing decision this definition produces is rejected on
// every attempt, and the refusal lands at assignment, after the exclusive
// board lease has been taken, not at admission where the contradiction was
// written.
//
// Only a DECLARED timeout is judged. A task that declares none is not
// silently held to hilpolicy.DefaultSeconds here: that 30s is the policy
// layer's choice for an app that stated nothing, not a number the reviewed
// definition claims, and refusing a definition for a default some other
// package picks is a different rule from holding a definition to what it
// says. Undeclared stays a real answer, the same line HandoffBoundsDeclared
// and validateHandoffBounds already draw in this package.
//
// The safety maximum is left alone too, in the other direction: it is a cap
// that binds by being the smaller of several (hilspec.Decide takes the
// minimum), so a safety maximum above the deadline is inert rather than
// contradictory, and the deadline simply ends the attempt first.
//
// This is an admission rule, applied where a manifest is read, not a re-check
// of a task already persisted against a reviewed digest: it reads a field
// outside the HIL block, and ValidateHILTaskMetadata is re-applied to held
// definitions by store.attempts and store.HeldYieldWork, where a new
// cross-field refusal would retire work already running. A definition
// admitted under an older rule keeps running.
//
// Precedent for holding a HIL number under the task deadline:
// checkSafeStepFitsTheDeadline, the sibling rule on the handoff bound, and
// boardclient.validHILTimingAssignment at the other end of this same
// contract.
func checkObservationTimeoutFitsTheDeadline(task Task) error {
	if task.HIL == nil || !task.HIL.TimeoutDeclared {
		return nil
	}
	if task.HIL.TimeoutSeconds > task.DeadlineSeconds {
		return fmt.Errorf("%w: HIL task %q declares a %ds observation timeout inside a %ds deadline; the timing decision it produces is refused at every assignment",
			ErrInvalidCatalog, task.Name, task.HIL.TimeoutSeconds, task.DeadlineSeconds)
	}
	return nil
}
