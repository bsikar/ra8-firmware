// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// bracketedReceipt is a receipt whose two host snapshots sit where the agent
// takes them, one just before the attempt and one just after, so each test
// below moves exactly the capture stamp it is about.
func bracketedReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	zero := 0
	now := time.Now().UTC()
	facts := func(at time.Time) HostFacts {
		return HostFacts{Cores: 2, RAMBytes: 1 << 30, RAMFreeBytes: 1 << 29, Load1: 0.5,
			LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: at}
	}
	return TerminalReceipt{
		SchemaVersion: Version, AssignmentID: "018f8b3a-1c2d-7e4f-8a1b-2c3d4e5f6a7b",
		AttemptID: "018f8b3a-1c2d-7e4f-9a1b-2c3d4e5f6a7c", AssignmentVersion: 1, FencingToken: 1,
		Outcome: "succeeded", ChildExitCode: &zero, EvidenceComplete: true, FinalLogSequence: 1,
		StartedAt: now, EndedAt: now.Add(10 * time.Second), DurationNS: int64(10 * time.Second),
		CatalogSHA256: strings.Repeat("a", 64), SourceSnapshotSHA256: strings.Repeat("b", 64),
		HostFactsAtStart: facts(now.Add(-20 * time.Millisecond)),
		HostFactsAtEnd:   facts(now.Add(10*time.Second + 20*time.Millisecond)),
		Steps: []StepSummary{
			{Name: "one", StartedAt: now, EndedAt: now.Add(4 * time.Second),
				DurationNS: int64(4 * time.Second), StdoutSHA256: emptyStreamDigest,
				StderrSHA256: emptyStreamDigest},
		},
	}
}

func TestSnapshotsTakenAroundTheAttemptAreAccepted(t *testing.T) {
	if err := bracketedReceipt(t).Validate(); err != nil {
		t.Fatalf("the agent's own bracketing snapshots were refused: %v", err)
	}
}

func TestSnapshotsTakenAtTheSameInstantAreAccepted(t *testing.T) {
	receipt := bracketedReceipt(t)
	receipt.HostFactsAtEnd.CapturedAt = receipt.HostFactsAtStart.CapturedAt
	if err := receipt.Validate(); err != nil {
		t.Fatalf("two snapshots sharing one stamp were refused: %v", err)
	}
}

func TestTheEndSnapshotTakenBeforeTheStartOneIsRefused(t *testing.T) {
	receipt := bracketedReceipt(t)
	receipt.HostFactsAtEnd.CapturedAt = receipt.HostFactsAtStart.CapturedAt.Add(-time.Millisecond)
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a reversed pair of snapshots was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "before the one at the start") {
		t.Fatalf("the refusal did not name the reversal: %v", err)
	}
}

func TestTheStartSnapshotTakenAfterTheAttemptEndedIsRefused(t *testing.T) {
	receipt := bracketedReceipt(t)
	late := receipt.EndedAt.Add(time.Minute)
	receipt.HostFactsAtStart.CapturedAt = late
	receipt.HostFactsAtEnd.CapturedAt = late.Add(time.Second)
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a start snapshot from after the attempt was accepted: %v", err)
	}
}

func TestTheEndSnapshotTakenBeforeTheAttemptStartedIsRefused(t *testing.T) {
	receipt := bracketedReceipt(t)
	early := receipt.StartedAt.Add(-time.Minute)
	receipt.HostFactsAtStart.CapturedAt = early.Add(-time.Second)
	receipt.HostFactsAtEnd.CapturedAt = early
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("an end snapshot from before the attempt was accepted: %v", err)
	}
}

// The readings are taken around the run rather than during it, so the rule
// allows the same clock disagreement every other stamp comparison here allows.
func TestASnapshotJustOutsideTheWindowIsAccepted(t *testing.T) {
	receipt := bracketedReceipt(t)
	receipt.HostFactsAtStart.CapturedAt = receipt.StartedAt.Add(-4 * time.Second)
	receipt.HostFactsAtEnd.CapturedAt = receipt.EndedAt.Add(4 * time.Second)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("snapshots inside the clock allowance were refused: %v", err)
	}
}

// HostFacts.Validate judges a snapshot on its own terms and refuses a zero
// stamp first, so this rule never sees one.
func TestAZeroCaptureStampIsStillRefusedByTheSnapshotItself(t *testing.T) {
	receipt := bracketedReceipt(t)
	receipt.HostFactsAtEnd.CapturedAt = time.Time{}
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a snapshot with no capture stamp was accepted: %v", err)
	}
}
