// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"fmt"
	"time"
)

// maxAttemptSpan is the longest any attempt can honestly have run. It is this
// package's own deadline ceiling read as a duration rather than a second
// number: a grant states RemainingMS and Assignment.Validate refuses anything
// above MaxDeadlineMS, and the agent refuses a catalog deadline above the same
// ceiling before it ever asks for work (agent.go, catalogDeadlineSeconds). So
// no attempt the plane issued was allowed to run longer than this, and a
// receipt claiming it did is describing an attempt no grant could have made.
const maxAttemptSpan = time.Duration(MaxDeadlineMS) * time.Millisecond

// checkAttemptFitsADeadlineAGrantCouldIssue holds a terminal receipt's own
// window and duration to the ceiling every grant is issued under.
//
// Nothing before this bounded either number from above. Validate holds EndedAt
// at or after StartedAt and DurationNS at or above zero; checkReportedDurations
// holds the duration to the span of its own stamps, which is a comparison of
// the receipt against itself and says nothing about how long the attempt was
// allowed to take; and the only span it refuses outright is a saturated one,
// about 292 years, which is where time.Time.Sub stops being able to measure at
// all. Between a second and three centuries, every span was admitted.
//
// A receipt is client JSON from a guest, and these two numbers are what the
// plane keeps. The duration lands in durable history as how long the work took
// and is what every later report of the attempt reads, and the window is what
// the host-facts bracket, the step timeline and the step-inside-the-attempt
// rule are all measured against. A receipt stating a fortnight passes every one
// of those rules by widening the frame they share: its steps sit inside the
// fortnight, its host facts bracket the fortnight, and the whole thing is
// internally consistent and cannot have happened.
//
// Only the attempt is judged here. A step is already held inside the attempt's
// window (receipt_step_order.go, checkStepIsWithinAttempt) and a step's
// duration to its own stamps (receipt_duration.go), so bounding the attempt
// bounds every step it reports, and a second rule per step would refuse the
// same receipt twice with a less useful message.
//
// The allowance is the package's clock disagreement, for the same reason the
// duration rule spends it: the stamps come from a wall clock read at two
// moments and the deadline is enforced against a different one, so an attempt
// that ran right up to its deadline may report a span a few nanoseconds past
// it. The rule is one-sided; an attempt shorter than the ceiling is the
// ordinary case and says nothing.
func checkAttemptFitsADeadlineAGrantCouldIssue(receipt TerminalReceipt) error {
	allowed := maxAttemptSpan + clockDisagreement
	if span := receipt.EndedAt.Sub(receipt.StartedAt); span > allowed {
		return fmt.Errorf("%w: receipt spans %s, longer than the %s any grant can allow",
			ErrInvalid, span, maxAttemptSpan)
	}
	if receipt.DurationNS > allowed.Nanoseconds() {
		return fmt.Errorf("%w: receipt reports %dns, longer than the %s any grant can allow",
			ErrInvalid, receipt.DurationNS, maxAttemptSpan)
	}
	return nil
}
