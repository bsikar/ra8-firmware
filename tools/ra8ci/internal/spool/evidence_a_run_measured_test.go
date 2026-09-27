// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// digestOfNothing is what the executor's digest writer reports for a step
// that printed nothing at all: the SHA-256 of the empty string.
const digestOfNothing = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

func measuredStep(name string) executor.StepResult {
	return executor.StepResult{
		Name:         name,
		StdoutSHA256: strings.Repeat("a", 64),
		StderrSHA256: strings.Repeat("b", 64),
		StdoutBytes:  12,
		StderrBytes:  0,
	}
}

func resultOf(steps ...executor.StepResult) executor.Result {
	return executor.Result{TaskName: "unit-tests", Steps: steps}
}

func TestAMeasuredRunIsFrozen(t *testing.T) {
	if err := checkTheEvidenceWasMeasured(resultOf(measuredStep("build"), measuredStep("test"))); err != nil {
		t.Fatalf("a measured run was refused: %v", err)
	}
}

func TestARunWithNoStepsIsFrozen(t *testing.T) {
	if err := checkTheEvidenceWasMeasured(resultOf()); err != nil {
		t.Fatalf("a result carrying no steps was refused: %v", err)
	}
}

func TestASilentStepIsFrozen(t *testing.T) {
	// The digest of nothing is still a digest, and a step that printed
	// nothing is an ordinary step.
	silent := measuredStep("build")
	silent.StdoutSHA256, silent.StderrSHA256 = digestOfNothing, digestOfNothing
	silent.StdoutBytes, silent.StderrBytes = 0, 0
	if err := checkTheEvidenceWasMeasured(resultOf(silent)); err != nil {
		t.Fatalf("a silent step was refused: %v", err)
	}
}

func TestAnUnmeasuredStdoutDigestIsRefused(t *testing.T) {
	step := measuredStep("build")
	step.StdoutSHA256 = ""
	if err := checkTheEvidenceWasMeasured(resultOf(step)); !errors.Is(err, errUnmeasuredEvidence) {
		t.Fatalf("a step stating no stdout digest was frozen: %v", err)
	}
}

func TestAnUnmeasuredStderrDigestIsRefused(t *testing.T) {
	step := measuredStep("build")
	step.StderrSHA256 = strings.Repeat("c", 63)
	if err := checkTheEvidenceWasMeasured(resultOf(step)); !errors.Is(err, errUnmeasuredEvidence) {
		t.Fatalf("a step stating a short stderr digest was frozen: %v", err)
	}
}

func TestAnUppercaseDigestIsRefused(t *testing.T) {
	// The column the plane files this into is matched lowercase, so an
	// uppercase digest is refused rather than folded.
	step := measuredStep("build")
	step.StdoutSHA256 = strings.Repeat("A", 64)
	if err := checkTheEvidenceWasMeasured(resultOf(step)); !errors.Is(err, errUnmeasuredEvidence) {
		t.Fatalf("an uppercase digest was frozen: %v", err)
	}
}

func TestANegativeByteCountIsRefused(t *testing.T) {
	step := measuredStep("test")
	step.StderrBytes = -1
	if err := checkTheEvidenceWasMeasured(resultOf(step)); !errors.Is(err, errUnmeasuredEvidence) {
		t.Fatalf("a step stating a negative byte count was frozen: %v", err)
	}
}

func TestTheRefusalNamesTheStep(t *testing.T) {
	step := measuredStep("golden-tests")
	step.StdoutSHA256 = "not-a-digest"
	err := checkTheEvidenceWasMeasured(resultOf(measuredStep("build"), step))
	if err == nil || !strings.Contains(err.Error(), `step 1 "golden-tests"`) {
		t.Fatalf("the refusal did not name the step: %v", err)
	}
}

func TestACountDisagreeingWithTheDigestIsStillFrozen(t *testing.T) {
	// Deliberately not this package's rule: the server states the pair in
	// full, where the whole record is in hand.
	step := measuredStep("build")
	step.StdoutSHA256, step.StdoutBytes = digestOfNothing, 4096
	if err := checkTheEvidenceWasMeasured(resultOf(step)); err != nil {
		t.Fatalf("the freeze judged a pair it does not own: %v", err)
	}
}
