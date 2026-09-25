// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"fmt"
	"math"
	"time"
)

// clockDisagreement is how far a reported duration may exceed the span of the
// stamps it is reported with before the two are a contradiction rather than two
// clocks disagreeing. They genuinely disagree: the executor measures a duration
// from a pair of monotonic readings and stamps the same attempt with their wall
// clock values (executor.go, runTask and runStep), and the two differ by a small
// number of nanoseconds on every real run. The store already fixes this same
// allowance at five seconds where it judges HIL step evidence
// (store/board_hil_completion.go), so the agent path states the number the board
// path already states rather than inventing a second one.
const clockDisagreement = 5 * time.Second

// checkReportedDurations holds every duration a terminal receipt states to the
// pair of stamps it is stated with. A receipt is client JSON, so the three
// numbers arrive independently, and nothing before this compared them: Validate
// judged a duration only for being non-negative, and the store judged a step's
// duration against the receipt's total rather than against the step's own
// stamps, so a step reporting the whole attempt's hour between stamps a
// millisecond apart passed every check in the tree. The duration is the number
// that lands in durable history and in every later report of how long the work
// took.
//
// The rule is one-sided on purpose. A duration SHORTER than its span is
// ordinary: the stamps bracket the whole attempt while the duration may measure
// the child alone. Only a duration longer than the window it was measured in is
// a contradiction, because no clock can report more elapsed time than passed
// between the two readings that bound it.
func checkReportedDurations(receipt TerminalReceipt) error {
	if err := checkDurationFitsStamps("receipt", receipt.DurationNS, receipt.StartedAt, receipt.EndedAt); err != nil {
		return err
	}
	for _, step := range receipt.Steps {
		if err := checkDurationFitsStamps("step "+step.Name, step.DurationNS, step.StartedAt, step.EndedAt); err != nil {
			return err
		}
	}
	return nil
}

// checkDurationFitsStamps refuses a duration longer than the span it names, and
// refuses a span too wide to measure at all. time.Time.Sub saturates at MaxInt64
// rather than reporting an overflow, so a pair of stamps more than about 292
// years apart yields the largest representable span and would admit any duration
// whatsoever. Callers reach this only after Validate has held ended at or after
// started, so a saturated result means an absurd span and never a reversed one,
// and the subtraction below cannot overflow: both sides are non-negative.
func checkDurationFitsStamps(subject string, durationNS int64, started, ended time.Time) error {
	span := ended.Sub(started)
	if span == time.Duration(math.MaxInt64) {
		return fmt.Errorf("%w: %s spans longer than a clock can measure", ErrInvalid, subject)
	}
	if durationNS-span.Nanoseconds() > clockDisagreement.Nanoseconds() {
		return fmt.Errorf("%w: %s reports %dns between stamps %dns apart",
			ErrInvalid, subject, durationNS, span.Nanoseconds())
	}
	return nil
}
