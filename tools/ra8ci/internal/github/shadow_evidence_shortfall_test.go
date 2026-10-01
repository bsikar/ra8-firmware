// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

// shortHeldEvidence is one insufficient task and a readiness written beside the
// accumulation rather than read from it, which is the only way the numbers on
// an insufficient line can disagree. renderedEvidence reads its readiness from
// the evidence and can never produce one.
func shortHeldEvidence(graded, indeterminate int) ShadowEvidence {
	return ShadowEvidence{
		Commits: []string{"aaa1", "bbb2"},
		Tasks: []TaskEvidence{{
			Task: "build", Observed: graded + indeterminate, Graded: graded,
			Agreed: graded, Indeterminate: indeterminate,
		}},
	}
}

func shortHeldReadiness(threshold int, short TaskShortfall) ShadowReadiness {
	return ShadowReadiness{
		Threshold:    threshold,
		Insufficient: []string{"build"},
		Shortfall:    []TaskShortfall{short},
	}
}

// mismatchedEvidencePage renders and requires a readiness refusal.
func mismatchedEvidencePage(t *testing.T, evidence ShadowEvidence, readiness ShadowReadiness) string {
	t.Helper()
	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, readiness)
	if err == nil {
		t.Fatalf("rendered a page that should have been refused:\n%s", page.String())
	}
	if !errors.Is(err, ErrShadowEvidenceMismatch) {
		t.Fatalf("refused with %v, want ErrShadowEvidenceMismatch", err)
	}
	return err.Error()
}

func TestAShortfallGradedOnSomethingElseIsRefused(t *testing.T) {
	evidence := shortHeldEvidence(1, 1)
	readiness := shortHeldReadiness(3, TaskShortfall{Task: "build", Graded: 2, Remaining: 1})

	refusal := mismatchedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "graded on 1 in the evidence and on 2 in its shortfall") {
		t.Fatalf("the refusal does not say which two numbers disagree: %s", refusal)
	}
}

func TestATaskHeldShortWithNothingMoreNeededIsRefused(t *testing.T) {
	// "graded on 2, 0 more needed" under a page that holds the required
	// checks: the line says the wait is over and the verdict says it is
	// not. TaskShortfall documents Remaining as always at least one.
	evidence := shortHeldEvidence(2, 0)
	readiness := shortHeldReadiness(2, TaskShortfall{Task: "build", Graded: 2, Remaining: 0})

	refusal := mismatchedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "held short with 0 more needed") {
		t.Fatalf("a hold that needs nothing more was not refused as one: %s", refusal)
	}
}

func TestAShortfallThatDoesNotReachTheThresholdIsRefused(t *testing.T) {
	// 1 graded and 1 more needed, on a page whose first line says the
	// threshold is 5.
	evidence := shortHeldEvidence(1, 1)
	readiness := shortHeldReadiness(5, TaskShortfall{Task: "build", Graded: 1, Remaining: 1})

	refusal := mismatchedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "not the threshold 5") {
		t.Fatalf("the refusal does not name the threshold the page was read at: %s", refusal)
	}
}

func TestTheEvidenceIsReadBeforeTheThreshold(t *testing.T) {
	// A shortfall disagreeing with the accumulation on both counts is
	// refused for the accumulation: that is the contradiction on the line
	// itself, beside the pairing counts it is printed with.
	evidence := shortHeldEvidence(1, 1)
	readiness := shortHeldReadiness(9, TaskShortfall{Task: "build", Graded: 4, Remaining: 1})

	refusal := mismatchedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "in the evidence") {
		t.Fatalf("the threshold was read first: %s", refusal)
	}
	if strings.Contains(refusal, "threshold 9") {
		t.Fatalf("refused for the threshold rather than for the evidence: %s", refusal)
	}
}

func TestTheShortfallIsReadBeforeTheCommitsTheSameLineNames(t *testing.T) {
	// Both are on the insufficient line. The numbers are read first: a
	// shortfall belonging to another accumulation is why the commits under
	// it would be the wrong ones to go back to.
	evidence := ShadowEvidence{
		Commits: []string{"aaa1"},
		Tasks: []TaskEvidence{{
			Task: "build", Observed: 2, Graded: 1, Agreed: 1, Indeterminate: 1,
			IndeterminateCommits: []string{"never-accumulated"},
		}},
	}
	readiness := shortHeldReadiness(3, TaskShortfall{Task: "build", Graded: 2, Remaining: 1})

	refusal := mismatchedEvidencePage(t, evidence, readiness)
	if strings.Contains(refusal, "never-accumulated") {
		t.Fatalf("the commits were read before the numbers: %s", refusal)
	}
}

// A conflicting task carries no shortfall on purpose, so there is nothing here
// to add up and nothing to refuse.
func TestAConflictingTaskIsNotHeldToAnyArithmetic(t *testing.T) {
	evidence := ShadowEvidence{
		Commits: []string{"aaa1"},
		Tasks: []TaskEvidence{{
			Task: "build", Observed: 1, Graded: 1, Conflicting: 1,
			ConflictingCommits: []string{"aaa1"},
		}},
	}
	readiness := ShadowReadiness{Threshold: 9, Conflicting: []string{"build"}}

	var page strings.Builder
	if err := RenderShadowEvidence(&page, evidence, readiness); err != nil {
		t.Fatalf("refused a conflicting task for arithmetic it does not carry: %v", err)
	}
	if strings.Contains(page.String(), "more needed") {
		t.Fatalf("a conflicting task was given a remaining count:\n%s", page.String())
	}
}

// The shortfall a readiness reads from its own accumulation always adds up, at
// any threshold. This is the shape the check exists to let through.
func TestAReadinessReadFromItsOwnEvidenceAlwaysAddsUp(t *testing.T) {
	for _, threshold := range []int{1, 2, 5, 40} {
		evidence, readiness := renderedEvidence(t, threshold,
			TaskEvidence{Task: "build", Observed: 2, Graded: 1, Agreed: 1, Indeterminate: 1,
				IndeterminateCommits: []string{renderEvidenceCommitB}},
			TaskEvidence{Task: "lint", Observed: 2, Graded: 2, Agreed: 2},
		)
		var page strings.Builder
		if err := RenderShadowEvidence(&page, evidence, readiness); err != nil {
			t.Fatalf("threshold %d: refused a readiness read from its own evidence: %v", threshold, err)
		}
	}
}
