// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

func runnerFacts(at time.Time) protocol.HostFacts {
	return protocol.HostFacts{Cores: 2, RAMBytes: 1 << 30, RAMFreeBytes: 1 << 29,
		Load1: 0.5, LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: at.UTC()}
}

func readingFixture() (time.Time, protocol.HostFacts) {
	start := time.Date(2026, 9, 26, 15, 0, 0, 0, time.UTC)
	return start, runnerFacts(start)
}

func TestEndReadingTakenAfterTheWorkIsUsed(t *testing.T) {
	start, startFacts := readingFixture()
	measured := runnerFacts(start.Add(90 * time.Second))
	facts, atEnd := endHostFacts(startFacts, measured, nil)
	if !atEnd {
		t.Fatalf("a good end reading must be reported as measured at the end")
	}
	if !facts.CapturedAt.Equal(measured.CapturedAt) || facts.RAMFreeBytes != measured.RAMFreeBytes {
		t.Fatalf("the measured reading must be the one carried, got %+v", facts)
	}
}

func TestEndReadingThatFailedFallsBackToTheStart(t *testing.T) {
	start, startFacts := readingFixture()
	facts, atEnd := endHostFacts(startFacts, protocol.HostFacts{}, errors.New("open /proc/meminfo: too many open files"))
	if atEnd {
		t.Fatalf("a reading that failed is not a measurement of the end")
	}
	if !facts.CapturedAt.Equal(start.UTC()) {
		t.Fatalf("the start snapshot must stand in, got %v", facts.CapturedAt)
	}
}

func TestFallbackCarriesTheStartSnapshotUnchanged(t *testing.T) {
	_, startFacts := readingFixture()
	facts, _ := endHostFacts(startFacts, protocol.HostFacts{}, errors.New("read /proc/loadavg: interrupted"))
	if facts != startFacts {
		t.Fatalf("the stand-in must be the start reading itself, got %+v", facts)
	}
}

func TestInvalidEndReadingFallsBack(t *testing.T) {
	start, startFacts := readingFixture()
	for name, measured := range map[string]protocol.HostFacts{
		"no capture stamp": func() protocol.HostFacts {
			facts := runnerFacts(start.Add(time.Minute))
			facts.CapturedAt = time.Time{}
			return facts
		}(),
		"no cores": func() protocol.HostFacts {
			facts := runnerFacts(start.Add(time.Minute))
			facts.Cores = 0
			return facts
		}(),
		"free above total": func() protocol.HostFacts {
			facts := runnerFacts(start.Add(time.Minute))
			facts.RAMFreeBytes = facts.RAMBytes + 1
			return facts
		}(),
		"unnamed load kind": func() protocol.HostFacts {
			facts := runnerFacts(start.Add(time.Minute))
			facts.LoadKind = ""
			return facts
		}(),
	} {
		if measured.Validate() == nil {
			t.Fatalf("%s: fixture must be an invalid reading", name)
		}
		facts, atEnd := endHostFacts(startFacts, measured, nil)
		if atEnd || facts != startFacts {
			t.Fatalf("%s: an invalid reading must fall back to the start snapshot", name)
		}
	}
}

func TestEndReadingBeforeTheStartFallsBack(t *testing.T) {
	start, startFacts := readingFixture()
	measured := runnerFacts(start.Add(-time.Nanosecond))
	facts, atEnd := endHostFacts(startFacts, measured, nil)
	if atEnd || facts != startFacts {
		t.Fatalf("a reading the receipt cannot carry must fall back, got %+v %v", facts, atEnd)
	}
}

func TestEndReadingStampedAtTheStartIsKept(t *testing.T) {
	start, startFacts := readingFixture()
	measured := runnerFacts(start)
	measured.RAMFreeBytes = 1 << 28
	facts, atEnd := endHostFacts(startFacts, measured, nil)
	if !atEnd || facts.RAMFreeBytes != 1<<28 {
		t.Fatalf("an equal stamp is in order and the reading stands, got %+v %v", facts, atEnd)
	}
}

func TestWithoutEndReadingClearsTheEvidenceFlag(t *testing.T) {
	receipt := protocol.TerminalReceipt{Outcome: "failed", EvidenceComplete: true}
	if withoutEndReading(receipt).EvidenceComplete {
		t.Fatalf("a repeated start snapshot is not complete evidence")
	}
}

func TestWithoutEndReadingDowngradesAGreenAttempt(t *testing.T) {
	receipt := protocol.TerminalReceipt{Outcome: "succeeded", EvidenceComplete: true}
	marked := withoutEndReading(receipt)
	if marked.Outcome != "failed" || marked.EvidenceComplete {
		t.Fatalf("succeeded is defined as complete evidence, got %+v", marked)
	}
}

func TestWithoutEndReadingKeepsAChildVerdict(t *testing.T) {
	for _, outcome := range []string{"timed_out", "cancelled"} {
		marked := withoutEndReading(protocol.TerminalReceipt{Outcome: outcome, EvidenceComplete: true})
		if marked.Outcome != outcome || marked.EvidenceComplete {
			t.Fatalf("%s is a verdict about the child, got %+v", outcome, marked)
		}
	}
}

func TestWithoutEndReadingInventsNoErrorCode(t *testing.T) {
	marked := withoutEndReading(protocol.TerminalReceipt{Outcome: "succeeded", EvidenceComplete: true})
	if marked.ErrorCode != "" {
		t.Fatalf("the agent vocabulary is closed, got %q", marked.ErrorCode)
	}
	kept := withoutEndReading(protocol.TerminalReceipt{Outcome: "failed", ErrorCode: "log_upload_error"})
	if kept.ErrorCode != "log_upload_error" {
		t.Fatalf("an existing code must survive, got %q", kept.ErrorCode)
	}
}

// TestReceiptSurvivesALostEndReading is the point of the whole file: a green
// attempt whose end reading failed still produces a receipt the plane accepts,
// where before the receipt was never built.
func TestReceiptSurvivesALostEndReading(t *testing.T) {
	start, startFacts := readingFixture()
	assignment := testAssignment()
	ran := start.Add(time.Second)
	result := executor.Result{TaskName: "checks", StartedAt: ran, EndedAt: ran.Add(30 * time.Second),
		Duration: 30 * time.Second, ExitCode: 0,
		Steps: []executor.StepResult{{Name: "ascii", StartedAt: ran, EndedAt: ran.Add(30 * time.Second),
			Duration: 30 * time.Second, ExitCode: 0,
			StdoutSHA256: strings.Repeat("c", 64), StderrSHA256: strings.Repeat("d", 64),
			StdoutBytes: 64, StderrBytes: 8}}}

	endFacts, atEnd := endHostFacts(startFacts, protocol.HostFacts{}, errors.New("read /proc/meminfo: cannot allocate memory"))
	if atEnd {
		t.Fatalf("fixture must lose the end reading")
	}
	receipt := terminalReceipt(assignment, result, startFacts, endFacts, 7, nil, nil, nil)
	if receipt.Outcome != "succeeded" || !receipt.EvidenceComplete {
		t.Fatalf("fixture must start green, got %s %v", receipt.Outcome, receipt.EvidenceComplete)
	}
	receipt = withoutEndReading(receipt)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the plane must accept the downgraded receipt: %v", err)
	}
	if receipt.Outcome != "failed" || receipt.EvidenceComplete || receipt.FinalLogSequence != 7 ||
		len(receipt.Steps) != 1 || receipt.Steps[0].Name != "ascii" {
		t.Fatalf("the report itself must survive, got %+v", receipt)
	}
}

// TestGreenReceiptKeepsItsVerdictWhenTheReadingLands pins the other direction:
// the rule costs an ordinary attempt nothing.
func TestGreenReceiptKeepsItsVerdictWhenTheReadingLands(t *testing.T) {
	start, startFacts := readingFixture()
	assignment := testAssignment()
	ran := start.Add(time.Second)
	result := executor.Result{TaskName: "checks", StartedAt: ran, EndedAt: ran.Add(20 * time.Second),
		Duration: 20 * time.Second, ExitCode: 0,
		Steps: []executor.StepResult{{Name: "ascii", StartedAt: ran, EndedAt: ran.Add(20 * time.Second),
			Duration: 20 * time.Second, ExitCode: 0,
			StdoutSHA256: strings.Repeat("c", 64), StderrSHA256: strings.Repeat("d", 64),
			StdoutBytes: 64, StderrBytes: 8}}}

	endFacts, atEnd := endHostFacts(startFacts, runnerFacts(ran.Add(21*time.Second)), nil)
	if !atEnd {
		t.Fatalf("fixture must keep the end reading")
	}
	receipt := terminalReceipt(assignment, result, startFacts, endFacts, 3, nil, nil, nil)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("an ordinary receipt must validate: %v", err)
	}
	if receipt.Outcome != "succeeded" || !receipt.EvidenceComplete {
		t.Fatalf("a measured end reading leaves the verdict alone, got %+v", receipt)
	}
}
