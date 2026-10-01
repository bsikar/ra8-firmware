// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

func endedStep(name string, timedOut, cancelled bool) executor.StepResult {
	step := measuredStep(name)
	step.TimedOut, step.Cancelled = timedOut, cancelled
	return step
}

func TestAStepThatSimplyRanIsFrozen(t *testing.T) {
	if err := checkEachStepEndedOneWay(resultOf(endedStep("build", false, false))); err != nil {
		t.Fatalf("an ordinary step was refused: %v", err)
	}
}

func TestAStepThatRanOutOfTimeIsFrozen(t *testing.T) {
	if err := checkEachStepEndedOneWay(resultOf(endedStep("test", true, false))); err != nil {
		t.Fatalf("a timed out step was refused: %v", err)
	}
}

func TestAStepThatWasCalledOffIsFrozen(t *testing.T) {
	if err := checkEachStepEndedOneWay(resultOf(endedStep("test", false, true))); err != nil {
		t.Fatalf("a cancelled step was refused: %v", err)
	}
}

func TestAResultWithNoStepsEndsNoWay(t *testing.T) {
	if err := checkEachStepEndedOneWay(resultOf()); err != nil {
		t.Fatalf("a result carrying no steps was refused: %v", err)
	}
}

func TestTwoStepsEndingDifferentWaysAreFrozen(t *testing.T) {
	// One step out of time and a later one called off is an ordinary run
	// that came apart: each step still states one ending.
	result := resultOf(endedStep("build", true, false), endedStep("test", false, true))
	if err := checkEachStepEndedOneWay(result); err != nil {
		t.Fatalf("a run whose steps ended differently was refused: %v", err)
	}
}

func TestAStepEndingTwoWaysIsRefused(t *testing.T) {
	result := resultOf(endedStep("build", true, true))
	if err := checkEachStepEndedOneWay(result); !errors.Is(err, errContradictoryEnding) {
		t.Fatalf("a step stating both endings was frozen: %v", err)
	}
}

func TestALaterStepEndingTwoWaysIsRefused(t *testing.T) {
	result := resultOf(endedStep("build", false, false), endedStep("test", true, true))
	if err := checkEachStepEndedOneWay(result); !errors.Is(err, errContradictoryEnding) {
		t.Fatalf("a later step stating both endings was frozen: %v", err)
	}
}

func TestTheEndingRefusalNamesTheStep(t *testing.T) {
	err := checkEachStepEndedOneWay(resultOf(endedStep("build", false, false), endedStep("package", true, true)))
	if err == nil {
		t.Fatal("a step stating both endings was frozen")
	}
	if !strings.Contains(err.Error(), "package") {
		t.Fatalf("the refusal %q does not name the step", err)
	}
}

func TestTheAttemptLevelPairIsLeftToThePlane(t *testing.T) {
	// offlineInput does not refuse an attempt stating both, it reads
	// TimedOut first and files the run as timed out. Refusing it here would
	// throw away evidence the plane accepts.
	result := resultOf(endedStep("build", false, false))
	result.TimedOut, result.Cancelled = true, true
	if err := checkEachStepEndedOneWay(result); err != nil {
		t.Fatalf("an attempt stating both endings was refused: %v", err)
	}
}
