// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// attemptSpanReceipt is a receipt that passes every other rule, so each test
// below changes exactly the stamp or duration it is about.
func attemptSpanReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	zero := 0
	now := time.Now().UTC()
	facts := HostFacts{Cores: 2, RAMBytes: 1 << 30, RAMFreeBytes: 1 << 29, Load1: 0.5,
		LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: now}
	return TerminalReceipt{
		SchemaVersion: Version, AssignmentID: "018f8b3a-1c2d-7e4f-8a1b-2c3d4e5f6a7b",
		AttemptID: "018f8b3a-1c2d-7e4f-9a1b-2c3d4e5f6a7c", AssignmentVersion: 1, FencingToken: 1,
		Outcome: "succeeded", ChildExitCode: &zero, EvidenceComplete: true,
		StartedAt: now, EndedAt: now.Add(time.Second), DurationNS: int64(time.Second),
		CatalogSHA256: strings.Repeat("a", 64), SourceSnapshotSHA256: strings.Repeat("b", 64),
		HostFactsAtStart: facts, HostFactsAtEnd: facts,
		Steps: []StepSummary{{Name: "one", StartedAt: now, EndedAt: now.Add(time.Second),
			DurationNS: int64(time.Second)}},
	}
}

// widen moves the attempt's end, its duration, its end-side host facts and its
// single step's end together, so the receipt stays consistent with itself and
// only its total length changes.
func widen(receipt *TerminalReceipt, span time.Duration) {
	receipt.EndedAt = receipt.StartedAt.Add(span)
	receipt.DurationNS = int64(span)
	receipt.HostFactsAtEnd.CapturedAt = receipt.EndedAt
	receipt.Steps[0].EndedAt = receipt.EndedAt
	receipt.Steps[0].DurationNS = int64(span)
}

func TestTheCeilingIsTheDeadlineCeilingReadAsADuration(t *testing.T) {
	if maxAttemptSpan != 24*time.Hour {
		t.Fatalf("maxAttemptSpan = %s, want the 24h MaxDeadlineMS ceiling", maxAttemptSpan)
	}
}

func TestAnOrdinaryAttemptIsNotJudgedOnItsLength(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a one-second attempt was refused: %v", err)
	}
}

func TestAnAttemptRunningMostOfItsDeadlineIsAccepted(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	widen(&receipt, 23*time.Hour)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("an attempt inside the ceiling was refused: %v", err)
	}
}

func TestAnAttemptExactlyAtTheCeilingIsAccepted(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	widen(&receipt, maxAttemptSpan)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("an attempt that ran to its deadline was refused: %v", err)
	}
}

// The two clocks are read at different moments, so an attempt that ran right up
// to its deadline may report a span a little past it.
func TestTheClockAllowanceIsSpentOnTheCeilingToo(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	widen(&receipt, maxAttemptSpan+clockDisagreement)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the package's clock allowance was not spent: %v", err)
	}
}

func TestASpanPastTheAllowanceIsRefused(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	widen(&receipt, maxAttemptSpan+clockDisagreement+time.Second)
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "any grant can allow") {
		t.Fatalf("a span past the ceiling was accepted: %v", err)
	}
}

func TestAFortnightLongAttemptIsRefused(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	widen(&receipt, 14*24*time.Hour)
	if !errors.Is(receipt.Validate(), ErrInvalid) {
		t.Fatal("an attempt no grant could issue was accepted")
	}
}

// The duration is the number that lands in durable history, so it is judged on
// its own rather than only through the stamps it sits beside.
func TestADurationPastTheCeilingIsRefusedOnItsOwn(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	widen(&receipt, maxAttemptSpan)
	receipt.DurationNS = int64(maxAttemptSpan + time.Hour)
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatal("a duration no grant could allow was accepted")
	}
}

// checkReportedDurations runs first and its message is the one a reader needs
// for a receipt whose stamps cannot be subtracted at all.
func TestAnUnmeasurableSpanStillReportsItsOwnRefusal(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	receipt.EndedAt = receipt.StartedAt.Add(time.Duration(1 << 62)).Add(time.Duration(1 << 62))
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("an unmeasurable span was accepted: %v", err)
	}
}

// A shorter attempt is the ordinary case: the stamps bracket the whole attempt
// while the duration may measure the child alone.
func TestADurationShorterThanItsSpanIsStillAccepted(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	widen(&receipt, time.Hour)
	receipt.DurationNS = int64(time.Minute)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a duration inside its span was refused: %v", err)
	}
}

func TestTheRuleReadsTheAttemptDirectly(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	widen(&receipt, 30*24*time.Hour)
	if err := checkAttemptFitsADeadlineAGrantCouldIssue(receipt); err == nil {
		t.Fatal("the rule accepted a month-long attempt")
	}
	widen(&receipt, time.Minute)
	if err := checkAttemptFitsADeadlineAGrantCouldIssue(receipt); err != nil {
		t.Fatalf("the rule refused a minute-long attempt: %v", err)
	}
}
