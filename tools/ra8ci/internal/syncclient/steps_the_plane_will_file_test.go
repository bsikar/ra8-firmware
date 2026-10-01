// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

func stepped(names ...string) spool.Entry {
	steps := make([]executor.StepResult, 0, len(names))
	for _, name := range names {
		steps = append(steps, executor.StepResult{Name: name})
	}
	finished := time.Unix(200, 0).UTC()
	return spool.Entry{
		ID:         "0123456789abcdef0123456789abcdef",
		StartedAt:  time.Unix(100, 0).UTC(),
		FinishedAt: &finished,
		Result:     &executor.Result{Steps: steps},
	}
}

func TestTheStepsAReviewedRunStatesAreUploaded(t *testing.T) {
	if err := checkUploadedStepsAreOnesThePlaneWillFile(stepped("build", "vet", "test")); err != nil {
		t.Fatalf("named steps refused: %v", err)
	}
}

func TestARecordWithNoResultStatesNoSteps(t *testing.T) {
	entry := stepped()
	entry.Result = nil
	if err := checkUploadedStepsAreOnesThePlaneWillFile(entry); err != nil {
		t.Fatalf("a record without a result was judged for steps: %v", err)
	}
}

func TestARecordCarryingNoStepsIsUploaded(t *testing.T) {
	if err := checkUploadedStepsAreOnesThePlaneWillFile(stepped()); err != nil {
		t.Fatalf("a stepless record refused: %v", err)
	}
}

func TestTheLastStepThePlaneFilesIsUploaded(t *testing.T) {
	names := make([]string, 0, maxFilableSteps)
	for i := 0; i < maxFilableSteps; i++ {
		names = append(names, fmt.Sprintf("step-%03d", i))
	}
	if err := checkUploadedStepsAreOnesThePlaneWillFile(stepped(names...)); err != nil {
		t.Fatalf("%d steps refused: %v", maxFilableSteps, err)
	}
}

func TestOneStepMoreThanThePlaneFilesStopsTheSweep(t *testing.T) {
	names := make([]string, 0, maxFilableSteps+1)
	for i := 0; i <= maxFilableSteps; i++ {
		names = append(names, fmt.Sprintf("step-%03d", i))
	}
	err := checkUploadedStepsAreOnesThePlaneWillFile(stepped(names...))
	if !errors.Is(err, ErrUnfilableSteps) {
		t.Fatalf("%d steps accepted: %v", maxFilableSteps+1, err)
	}
}

func TestAStepStatingNoNameStopsTheSweep(t *testing.T) {
	err := checkUploadedStepsAreOnesThePlaneWillFile(stepped("build", ""))
	if !errors.Is(err, ErrUnfilableSteps) {
		t.Fatalf("a nameless step accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "step 1") {
		t.Fatalf("the refusal did not name the step: %v", err)
	}
}

func TestAStepNameLongerThanTheColumnStopsTheSweep(t *testing.T) {
	if err := checkUploadedStepsAreOnesThePlaneWillFile(
		stepped(strings.Repeat("s", maxFilableStepKeyBytes))); err != nil {
		t.Fatalf("a name of exactly the bound refused: %v", err)
	}
	err := checkUploadedStepsAreOnesThePlaneWillFile(
		stepped(strings.Repeat("s", maxFilableStepKeyBytes+1)))
	if !errors.Is(err, ErrUnfilableSteps) {
		t.Fatalf("an oversized step name accepted: %v", err)
	}
}

func TestTheStepNameBoundIsBytesNotRunes(t *testing.T) {
	// 64 two-byte runes are 128 bytes and fit; 65 are 130 and do not.
	if err := checkUploadedStepsAreOnesThePlaneWillFile(stepped(strings.Repeat("é", 64))); err != nil {
		t.Fatalf("128 bytes of runes refused: %v", err)
	}
	err := checkUploadedStepsAreOnesThePlaneWillFile(stepped(strings.Repeat("é", 65)))
	if !errors.Is(err, ErrUnfilableSteps) {
		t.Fatalf("130 bytes of runes accepted: %v", err)
	}
}

func TestAStepNameNoTextColumnCanHoldStopsTheSweep(t *testing.T) {
	for _, name := range []string{"build\x00vet", "build\tvet", "build\nvet", "build\x7f", "build\u009c", "build\xff"} {
		if err := checkUploadedStepsAreOnesThePlaneWillFile(stepped(name)); !errors.Is(err, ErrUnfilableSteps) {
			t.Fatalf("step name %q accepted: %v", name, err)
		}
	}
}

func TestATextualStepNameOutsideASCIIIsUploaded(t *testing.T) {
	if err := checkUploadedStepsAreOnesThePlaneWillFile(stepped("вёрстка", "ビルド", "build ok")); err != nil {
		t.Fatalf("a textual name outside ASCII refused: %v", err)
	}
}

func TestTwoStepsUnderOneNameStopTheSweep(t *testing.T) {
	err := checkUploadedStepsAreOnesThePlaneWillFile(stepped("build", "vet", "build"))
	if !errors.Is(err, ErrUnfilableSteps) {
		t.Fatalf("a repeated step name accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "step 2") {
		t.Fatalf("the refusal did not name the repeat: %v", err)
	}
}

func TestTheStepDoorDoesNotChooseACatalog(t *testing.T) {
	// offlineInput holds each step against the reviewed definition's own
	// step list. That needs the catalog a spooled record carries only the
	// digest of, so a name no definition declares passes this door.
	if err := checkUploadedStepsAreOnesThePlaneWillFile(stepped("no-definition-declares-this")); err != nil {
		t.Fatalf("the door chose a catalog: %v", err)
	}
}
