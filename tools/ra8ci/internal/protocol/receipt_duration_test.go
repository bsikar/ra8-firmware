// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"math"
	"strings"
	"testing"
	"time"
)

// durationReceipt is a receipt that passes every other rule Validate states,
// so each test below changes exactly the number it is about.
func durationReceipt(t *testing.T) TerminalReceipt {
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

func TestReceiptDurationEqualToItsSpanIsAccepted(t *testing.T) {
	receipt := durationReceipt(t)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the executor's own equal duration was refused: %v", err)
	}
}

func TestReceiptDurationShorterThanItsSpanIsAccepted(t *testing.T) {
	receipt := durationReceipt(t)
	receipt.DurationNS = int64(400 * time.Millisecond)
	receipt.Steps[0].DurationNS = int64(300 * time.Millisecond)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a duration inside its span was refused: %v", err)
	}
}

func TestReceiptDurationLongerThanItsSpanIsRefused(t *testing.T) {
	receipt := durationReceipt(t)
	receipt.DurationNS = int64(time.Second + clockDisagreement + time.Millisecond)
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a duration past its own stamps was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "receipt") {
		t.Fatalf("refusal does not name the receipt: %v", err)
	}
}

func TestStepDurationLongerThanItsSpanIsRefused(t *testing.T) {
	receipt := durationReceipt(t)
	receipt.Steps[0].EndedAt = receipt.StartedAt.Add(10 * time.Millisecond)
	receipt.Steps[0].DurationNS = int64(clockDisagreement + time.Minute)
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step duration past its own stamps was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "one") {
		t.Fatalf("refusal does not name the step: %v", err)
	}
}

// A step that fits the receipt's total is still judged against its own stamps:
// the store compared a step only with the receipt, so a short step reporting
// most of the attempt's duration passed every check in the tree.
func TestStepDurationInsideTheReceiptTotalIsStillJudgedByItsOwnStamps(t *testing.T) {
	receipt := durationReceipt(t)
	receipt.EndedAt = receipt.StartedAt.Add(time.Hour)
	receipt.DurationNS = int64(time.Hour)
	receipt.Steps[0].EndedAt = receipt.StartedAt.Add(time.Millisecond)
	receipt.Steps[0].DurationNS = int64(time.Hour)
	if receipt.Steps[0].DurationNS > receipt.DurationNS {
		t.Fatal("fixture no longer fits inside the receipt total")
	}
	if !errors.Is(receipt.Validate(), ErrInvalid) {
		t.Fatal("a step measuring the whole attempt in one millisecond was accepted")
	}
}

// A duration the two clocks disagree about by less than the stated allowance is
// what every honest run reports, since the executor measures the monotonic pair
// and stamps the wall clock pair.
func TestDurationInsideTheClockAllowanceIsAccepted(t *testing.T) {
	receipt := durationReceipt(t)
	receipt.DurationNS = int64(time.Second) + 1
	receipt.Steps[0].DurationNS = int64(time.Second) + 1
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a nanosecond of clock disagreement was refused: %v", err)
	}
}

// The zero duration every receipt carries before the executor reports one, on
// the agent's substitution path, must stay acceptable.
func TestAbsentDurationIsAccepted(t *testing.T) {
	receipt := durationReceipt(t)
	receipt.DurationNS = 0
	receipt.Steps[0].DurationNS = 0
	if err := receipt.Validate(); err != nil {
		t.Fatalf("an unreported duration was refused: %v", err)
	}
}

// time.Time.Sub saturates rather than overflowing, so without this the widest
// stamps in the type admit any duration at all.
func TestSaturatedSpanIsRefused(t *testing.T) {
	receipt := durationReceipt(t)
	receipt.EndedAt = receipt.StartedAt.Add(time.Duration(math.MaxInt64)).Add(time.Hour)
	receipt.Steps[0].EndedAt = receipt.EndedAt
	if receipt.EndedAt.Sub(receipt.StartedAt) != time.Duration(math.MaxInt64) {
		t.Fatalf("fixture no longer saturates: %d", receipt.EndedAt.Sub(receipt.StartedAt))
	}
	receipt.DurationNS = math.MaxInt64
	if !errors.Is(receipt.Validate(), ErrInvalid) {
		t.Fatal("a span too wide to measure was accepted")
	}
}

// A reversed pair is still the outer rule's refusal, not this one's.
func TestReversedStampsStayTheExistingRefusal(t *testing.T) {
	receipt := durationReceipt(t)
	receipt.EndedAt = receipt.StartedAt.Add(-time.Second)
	receipt.DurationNS = 0
	if !errors.Is(receipt.Validate(), ErrInvalid) {
		t.Fatal("a receipt ending before it started was accepted")
	}
}
