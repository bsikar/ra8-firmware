// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// Every test here drives ValidateReviewedTask or Load, the public entry points
// the rule is wired into, so a missing call site fails here rather than passing
// on a direct helper call.

func TestTheWidestGrantableTaskNameIsAdmitted(t *testing.T) {
	task := filableTask()
	task.Name = strings.Repeat("a", maxGrantableTaskNameBytes)
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a task name of exactly %d bytes must be admitted: %v", maxGrantableTaskNameBytes, err)
	}
}

func TestATaskNamedWiderThanAGrantCanCarryIsRefused(t *testing.T) {
	task := filableTask()
	task.Name = strings.Repeat("a", maxGrantableTaskNameBytes+1)
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
	for _, want := range []string{"65 bytes", "64", "grant"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal must name %q, got %v", want, err)
		}
	}
}

func TestTheGapBetweenTheGrantAndHistoryIsClosed(t *testing.T) {
	// The shape this slice exists for: a name history files willingly and no
	// grant can carry. It was admitted before, which made the task
	// unreachable rather than refused.
	task := filableTask()
	task.Name = strings.Repeat("a", maxFilableNameBytes)
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a name only history could file must be refused, got %v", err)
	}
}

func TestTheGrantNameDoorDoesNotChooseAnAlphabet(t *testing.T) {
	// validName owns the alphabet and the empty name. This door states the
	// length and nothing else, so a refusal for a bad character must still
	// come from there and name the step or task, not the grant.
	task := filableTask()
	task.Name = "Selftest"
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
	if strings.Contains(err.Error(), "grant") {
		t.Fatalf("the alphabet is validName's rule, not this door's: %v", err)
	}
}

func TestTheGrantNameDoorSaysNothingAboutStepNames(t *testing.T) {
	// No grant carries a step name, so a step named wider than a grant could
	// carry is left to the history bound alone.
	task := filableTask()
	task.Steps = []Step{{
		Name:    strings.Repeat("s", maxGrantableTaskNameBytes+1),
		Program: DispatchShell,
		Args:    []string{"scripts/checks/format_tree" + ScriptPathSuffix},
	}}
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a step name inside the history bound must be admitted: %v", err)
	}
}

func TestTheEmbeddedCatalogNamesEveryTaskAGrantCanCarry(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must be admitted: %v", err)
	}
}
