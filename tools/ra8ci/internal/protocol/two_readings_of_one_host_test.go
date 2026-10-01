// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// oneHostReceipt is a receipt that passes every other rule, so each test below
// changes exactly the host field it is about.
func oneHostReceipt(t *testing.T) TerminalReceipt {
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

func TestTwoReadingsOfOneHostAreAccepted(t *testing.T) {
	receipt := oneHostReceipt(t)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("an honest pair was refused: %v", err)
	}
}

func TestAPairChangingOperatingSystemIsRefused(t *testing.T) {
	receipt := oneHostReceipt(t)
	receipt.HostFactsAtEnd.OS = "windows"
	receipt.HostFactsAtEnd.LoadKind = "cpu_busy_equivalent"
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "at the start") {
		t.Fatalf("a receipt naming two operating systems was accepted: %v", err)
	}
}

func TestAPairChangingArchitectureIsRefused(t *testing.T) {
	receipt := oneHostReceipt(t)
	receipt.HostFactsAtEnd.Arch = "arm64"
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "arm64") {
		t.Fatalf("a receipt naming two architectures was accepted: %v", err)
	}
}

// The start snapshot is not privileged: a pair disagreeing the other way is the
// same receipt read backwards.
func TestTheDisagreementIsRefusedFromEitherSide(t *testing.T) {
	receipt := oneHostReceipt(t)
	receipt.HostFactsAtStart.Arch = "arm64"
	if !errors.Is(receipt.Validate(), ErrInvalid) {
		t.Fatal("a pair disagreeing at the start was accepted")
	}
}

// Free memory and load are why there are two readings at all.
func TestTheNumbersThatMoveAreLeftAlone(t *testing.T) {
	receipt := oneHostReceipt(t)
	receipt.HostFactsAtEnd.RAMFreeBytes = 1 << 27
	receipt.HostFactsAtEnd.Load1 = 3.25
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a runner under load at the end was refused: %v", err)
	}
}

// A runner can legitimately be resized between readings, so the measured
// capacities are not held equal.
func TestAResizedRunnerIsStillOneHost(t *testing.T) {
	receipt := oneHostReceipt(t)
	receipt.HostFactsAtEnd.Cores = 8
	receipt.HostFactsAtEnd.RAMBytes = 1 << 32
	receipt.HostFactsAtEnd.RAMFreeBytes = 1 << 31
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a resized runner was refused: %v", err)
	}
}

// Each snapshot is still judged on its own terms first, so an unreportable
// value reports that rather than a disagreement.
func TestAMalformedSnapshotIsStillJudgedOnItsOwnTerms(t *testing.T) {
	receipt := oneHostReceipt(t)
	receipt.HostFactsAtEnd.Arch = "AMD64"
	if !errors.Is(receipt.Validate(), ErrInvalid) {
		t.Fatal("an unreportable arch was accepted")
	}
}

func TestBothWindowsSnapshotsAreOneHost(t *testing.T) {
	receipt := oneHostReceipt(t)
	receipt.HostFactsAtStart.OS, receipt.HostFactsAtEnd.OS = "windows", "windows"
	receipt.HostFactsAtStart.LoadKind = "cpu_busy_equivalent"
	receipt.HostFactsAtEnd.LoadKind = "cpu_busy_equivalent"
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a windows runner was refused: %v", err)
	}
}

func TestTheRuleReadsThePairDirectly(t *testing.T) {
	receipt := oneHostReceipt(t)
	if err := checkHostFactsNameOneHost(receipt); err != nil {
		t.Fatalf("the rule refused one host: %v", err)
	}
	receipt.HostFactsAtEnd.OS = "windows"
	if err := checkHostFactsNameOneHost(receipt); err == nil {
		t.Fatal("the rule accepted two hosts")
	}
}

func TestTheRuleNamesBothSidesInItsMessage(t *testing.T) {
	receipt := oneHostReceipt(t)
	receipt.HostFactsAtEnd.OS = "windows"
	err := checkHostFactsNameOneHost(receipt)
	if err == nil || !strings.Contains(err.Error(), "linux") || !strings.Contains(err.Error(), "windows") {
		t.Fatalf("the message must name what disagreed, got %v", err)
	}
}
