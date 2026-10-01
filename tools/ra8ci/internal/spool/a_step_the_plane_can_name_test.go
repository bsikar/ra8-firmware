// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// stepsNamed builds a result whose steps carry exactly the given names and are
// otherwise unremarkable, so each test states only the thing it is about.
func stepsNamed(names ...string) executor.Result {
	result := executor.Result{TaskName: "build"}
	for _, name := range names {
		result.Steps = append(result.Steps, executor.StepResult{Name: name})
	}
	return result
}

func TestAStepNamedPlainlyIsFrozen(t *testing.T) {
	if err := checkEachStepCanBeNamed(stepsNamed("configure", "compile", "link")); err != nil {
		t.Fatalf("plainly named steps refused: %v", err)
	}
}

func TestAResultStatingNoStepsNeedsNoNames(t *testing.T) {
	if err := checkEachStepCanBeNamed(executor.Result{TaskName: "build"}); err != nil {
		t.Fatalf("a result stating no steps refused: %v", err)
	}
}

func TestAStepStatingNoNameIsRefused(t *testing.T) {
	err := checkEachStepCanBeNamed(stepsNamed("configure", ""))
	if !errors.Is(err, errUnnameableStep) {
		t.Fatalf("want errUnnameableStep, got %v", err)
	}
	if !strings.Contains(err.Error(), "step 1") {
		t.Fatalf("refusal does not point at the step: %v", err)
	}
}

func TestTwoStepsStatingOneNameAreRefused(t *testing.T) {
	err := checkEachStepCanBeNamed(stepsNamed("compile", "link", "compile"))
	if !errors.Is(err, errUnnameableStep) {
		t.Fatalf("want errUnnameableStep, got %v", err)
	}
	if !strings.Contains(err.Error(), "step 0") {
		t.Fatalf("refusal does not name the step that stated it first: %v", err)
	}
}

func TestANameAtTheBoundIsFrozen(t *testing.T) {
	if err := checkEachStepCanBeNamed(stepsNamed(strings.Repeat("s", maxMeasuredStepNameBytes))); err != nil {
		t.Fatalf("a name of exactly the bound refused: %v", err)
	}
}

func TestANameOverTheBoundIsRefused(t *testing.T) {
	err := checkEachStepCanBeNamed(stepsNamed(strings.Repeat("s", maxMeasuredStepNameBytes+1)))
	if !errors.Is(err, errUnnameableStep) {
		t.Fatalf("want errUnnameableStep, got %v", err)
	}
}

// The store sizes the column in bytes, so a name of 128 two-byte runes is over
// the bound even though it is 128 characters long.
func TestTheStepNameBoundIsBytesNotRunes(t *testing.T) {
	name := strings.Repeat("é", maxMeasuredStepNameBytes)
	if len([]rune(name)) != maxMeasuredStepNameBytes {
		t.Fatalf("fixture is %d runes, want %d", len([]rune(name)), maxMeasuredStepNameBytes)
	}
	err := checkEachStepCanBeNamed(stepsNamed(name))
	if !errors.Is(err, errUnnameableStep) {
		t.Fatalf("want errUnnameableStep for %d bytes, got %v", len(name), err)
	}
}

func TestAStepNamedWithANewlineIsRefused(t *testing.T) {
	err := checkEachStepCanBeNamed(stepsNamed("compile\nlink"))
	if !errors.Is(err, errUnnameableStep) {
		t.Fatalf("want errUnnameableStep, got %v", err)
	}
}

func TestAStepNamedWithANulIsRefused(t *testing.T) {
	err := checkEachStepCanBeNamed(stepsNamed("compile\x00"))
	if !errors.Is(err, errUnnameableStep) {
		t.Fatalf("want errUnnameableStep, got %v", err)
	}
}

func TestAStepNamedWithInvalidUTF8IsRefused(t *testing.T) {
	err := checkEachStepCanBeNamed(stepsNamed("compile\xff"))
	if !errors.Is(err, errUnnameableStep) {
		t.Fatalf("want errUnnameableStep, got %v", err)
	}
}

func TestAStepNamedWithADeleteIsRefused(t *testing.T) {
	err := checkEachStepCanBeNamed(stepsNamed("compile\x7f"))
	if !errors.Is(err, errUnnameableStep) {
		t.Fatalf("want errUnnameableStep, got %v", err)
	}
}

// A name a person can read is filed whatever alphabet it is written in; the
// rule is about what the column holds, not about English.
func TestAStepNamedInAnotherAlphabetIsFrozen(t *testing.T) {
	if err := checkEachStepCanBeNamed(stepsNamed("сборка", "コンパイル")); err != nil {
		t.Fatalf("a name in another alphabet refused: %v", err)
	}
}

func TestTheStepsAtTheCountBoundAreFrozen(t *testing.T) {
	names := make([]string, maxMeasurableSteps)
	for i := range names {
		names[i] = "step-" + string(rune('a'+i%26)) + strings.Repeat("x", i/26)
	}
	if err := checkEachStepCanBeNamed(stepsNamed(names...)); err != nil {
		t.Fatalf("a result of exactly the bound refused: %v", err)
	}
}

func TestMoreStepsThanThePlaneFilesAreRefused(t *testing.T) {
	names := make([]string, maxMeasurableSteps+1)
	for i := range names {
		names[i] = "step-" + string(rune('a'+i%26)) + strings.Repeat("x", i/26)
	}
	err := checkEachStepCanBeNamed(stepsNamed(names...))
	if !errors.Is(err, errUnnameableStep) {
		t.Fatalf("want errUnnameableStep, got %v", err)
	}
}

// The freeze judges the name as text and never as a member of a reviewed
// catalog: it holds only the digest of one, so a step named something no task
// definition declares is frozen here and left to the door that has the catalog.
func TestTheStepNameDoorDoesNotChooseACatalog(t *testing.T) {
	if err := checkEachStepCanBeNamed(stepsNamed("not-a-declared-step", "rm -rf /", "--flag")); err != nil {
		t.Fatalf("the freeze judged catalog membership: %v", err)
	}
}
