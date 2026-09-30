// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"testing"
)

// The safety maximum is the cap on a whole HIL attempt: the point at which the
// board is stopped whatever the task is doing. The observation timeout is how
// long the task says it needs to see what it came to see. A catalog that caps
// the attempt below its own observation has promised to kill the board before
// the task can finish, so every run of it either fails or races. That is a
// contradiction in the reviewed catalog, not a runtime condition, so it is
// refused at review.

// A declared timeout is the observation's own number, and the cap must cover it.
func TestASafetyMaximumUnderTheDeclaredTimeoutIsRefused(t *testing.T) {
	hil := *observedHILTask(t).HIL
	hil.TimeoutDeclared = true
	hil.TimeoutSeconds = 30
	hil.SafetyMaximumSeconds = 10
	hil.HandoffSafeStepSeconds = 10
	hil.HandoffRestoreProbeSeconds = 10

	if err := ValidateHILTaskMetadata(hil); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a cap below the observation it must cover was accepted: %v", err)
	}
}

// An undeclared timeout still has a number the runner will use, the thirty
// second fallback, and the cap is held to that rather than to zero. A task
// that declares nothing is the easiest place for this contradiction to hide.
func TestASafetyMaximumUnderTheFallbackTimeoutIsRefused(t *testing.T) {
	hil := *observedHILTask(t).HIL
	hil.TimeoutDeclared = false
	hil.TimeoutSeconds = 0
	hil.SafetyMaximumSeconds = 10
	hil.HandoffSafeStepSeconds = 10
	hil.HandoffRestoreProbeSeconds = 10

	if err := ValidateHILTaskMetadata(hil); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a cap below the fallback observation timeout was accepted: %v", err)
	}
}

// The boundary is equality: a cap exactly as long as the observation is the
// tightest honest answer, not a contradiction. And no cap at all is a task
// that has not asked for one, which is a different decision from asking for
// one that cannot hold.
func TestASafetyMaximumThatCoversTheObservationIsAccepted(t *testing.T) {
	for _, row := range []struct {
		name     string
		declared bool
		timeout  int
		maximum  int
	}{
		{"exactly at the declared timeout", true, 30, 30},
		{"above the declared timeout", true, 30, 120},
		{"exactly at the fallback", false, 0, 30},
		{"above the fallback", false, 0, 45},
		{"no cap asked for at all", false, 0, 0},
	} {
		t.Run(row.name, func(t *testing.T) {
			hil := *observedHILTask(t).HIL
			hil.TimeoutDeclared = row.declared
			hil.TimeoutSeconds = row.timeout
			hil.SafetyMaximumSeconds = row.maximum
			hil.HandoffSafeStepSeconds = 30
			hil.HandoffRestoreProbeSeconds = 30
			if row.maximum > 0 && row.maximum < 30 {
				t.Fatalf("fixture builds a refusal, not an acceptance")
			}
			if err := ValidateHILTaskMetadata(hil); err != nil {
				t.Fatalf("an honest cap was refused: %v", err)
			}
		})
	}
}

// Canonical JSON is what the catalog digest is taken over, so a value that
// stops halfway cannot be allowed to canonicalize into something shorter and
// well-formed. The walk reads the closing delimiter of everything it opened,
// and a truncated document is refused there rather than digested.
func TestCanonicalJSONRefusesAValueThatStopsHalfway(t *testing.T) {
	for _, raw := range []string{`{"a":1`, `[1`, `{"a":{"b":2}`, `[[1]`} {
		if _, err := CanonicalJSON([]byte(raw)); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("truncated document %s was canonicalized: %v", raw, err)
		}
	}
}
