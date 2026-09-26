// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
	"time"
)

// evidenceReceipt is a receipt that passes every other rule Validate states and
// carries one step with real stdout evidence, so each test below changes
// exactly the pair it is about.
func evidenceReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	zero := 0
	now := time.Now().UTC()
	output := sha256.Sum256([]byte("format: 3 files unchanged\n"))
	facts := HostFacts{Cores: 2, RAMBytes: 1 << 30, RAMFreeBytes: 1 << 29, Load1: 0.5,
		LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: now}
	return TerminalReceipt{
		SchemaVersion: Version, AssignmentID: "018f8b3a-1c2d-7e4f-8a1b-2c3d4e5f6a7b",
		AttemptID: "018f8b3a-1c2d-7e4f-9a1b-2c3d4e5f6a7c", AssignmentVersion: 1, FencingToken: 1,
		Outcome: "succeeded", ChildExitCode: &zero, EvidenceComplete: true, FinalLogSequence: 1,
		StartedAt: now, EndedAt: now.Add(time.Second), DurationNS: int64(time.Second),
		CatalogSHA256: strings.Repeat("a", 64), SourceSnapshotSHA256: strings.Repeat("b", 64),
		HostFactsAtStart: facts, HostFactsAtEnd: facts,
		Steps: []StepSummary{{Name: "format", StartedAt: now, EndedAt: now.Add(time.Second),
			DurationNS:   int64(time.Second),
			StdoutSHA256: hex.EncodeToString(output[:]), StdoutBytes: 26,
			StderrSHA256: emptyStreamDigest}},
	}
}

func TestRealOutputBesideItsByteCountIsAccepted(t *testing.T) {
	receipt := evidenceReceipt(t)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the executor's own log evidence was refused: %v", err)
	}
}

func TestEmptyStreamDigestWithNoBytesIsAccepted(t *testing.T) {
	receipt := evidenceReceipt(t)
	receipt.Steps[0].StdoutSHA256, receipt.Steps[0].StdoutBytes = emptyStreamDigest, 0
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a step that wrote nothing was refused: %v", err)
	}
}

func TestAbsentDigestsAreLeftAlone(t *testing.T) {
	receipt := evidenceReceipt(t)
	receipt.Steps[0].StdoutSHA256, receipt.Steps[0].StderrSHA256 = "", ""
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a receipt stating no digests was refused: %v", err)
	}
}

func TestNoBytesWithTheDigestOfSomeIsRefused(t *testing.T) {
	receipt := evidenceReceipt(t)
	receipt.Steps[0].StdoutBytes = 0
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("zero bytes beside the digest of real output were accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "step format states no stdout bytes") {
		t.Fatalf("the refusal does not name the step and stream: %v", err)
	}
}

func TestBytesWithTheDigestOfNoneIsRefused(t *testing.T) {
	receipt := evidenceReceipt(t)
	receipt.Steps[0].StderrBytes = 4096
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("real bytes beside the empty-stream digest were accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "step format states 4096 stderr bytes") {
		t.Fatalf("the refusal does not name the step, count and stream: %v", err)
	}
}

func TestMalformedDigestIsRefused(t *testing.T) {
	for _, digest := range []string{"deadbeef", strings.Repeat("A", 64), strings.Repeat("z", 64)} {
		receipt := evidenceReceipt(t)
		receipt.Steps[0].StdoutSHA256 = digest
		err := receipt.Validate()
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("malformed digest %q was accepted: %v", digest, err)
		}
		if !strings.Contains(err.Error(), "malformed stdout digest") {
			t.Fatalf("the refusal does not name the stream: %v", err)
		}
	}
}

func TestEveryStepIsJudged(t *testing.T) {
	receipt := evidenceReceipt(t)
	first := receipt.Steps[0]
	second := first
	second.Name = "second"
	second.StartedAt, second.EndedAt = first.EndedAt, first.EndedAt
	second.DurationNS = 0
	second.StdoutBytes = 0
	receipt.Steps = []StepSummary{first, second}
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a later step's contradiction was not judged: %v", err)
	}
	if !strings.Contains(err.Error(), "step second") {
		t.Fatalf("the refusal names the wrong step: %v", err)
	}
}

func TestEmptyStreamDigestIsTheKnownConstant(t *testing.T) {
	const known = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	if emptyStreamDigest != known {
		t.Fatalf("emptyStreamDigest = %s, want %s", emptyStreamDigest, known)
	}
}
