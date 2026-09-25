// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

// namedCommitEvidence builds an evidence value whose accumulation carries
// exactly the commits given, so a test can name a commit the page prints and
// decide for itself whether the accumulation looked at it. renderedEvidence
// accumulates nothing and is no use for that.
func namedCommitEvidence(commits []string, tasks ...TaskEvidence) ShadowEvidence {
	return ShadowEvidence{Commits: commits, Tasks: tasks}
}

// refusedEvidencePage renders and requires a refusal, returning its text.
func refusedEvidencePage(t *testing.T, evidence ShadowEvidence, readiness ShadowReadiness) string {
	t.Helper()
	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, readiness)
	if err == nil {
		t.Fatalf("rendered a page that should have been refused:\n%s", page.String())
	}
	if !errors.Is(err, ErrShadowEvidenceReportInvalid) {
		t.Fatalf("refused with %v, want ErrShadowEvidenceReportInvalid", err)
	}
	return err.Error()
}

func TestAConflictingTaskNamingACommitNobodyAccumulatedIsRefused(t *testing.T) {
	evidence := namedCommitEvidence([]string{"aaa1"}, TaskEvidence{
		Task: "build", Observed: 1, Graded: 1, Conflicting: 1,
		ConflictingCommits: []string{"bbb2"},
	})
	readiness := ShadowReadiness{Threshold: 1, Conflicting: []string{"build"}}

	refusal := refusedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "bbb2") || !strings.Contains(refusal, "not one of the accumulated commits") {
		t.Fatalf("refusal does not name the commit it never looked at: %s", refusal)
	}
	if !strings.Contains(refusal, "disagreed on") {
		t.Fatalf("refusal does not say which line it would have printed: %s", refusal)
	}
}

func TestAnInsufficientTaskNamingACommitNobodyAccumulatedIsRefused(t *testing.T) {
	evidence := namedCommitEvidence([]string{"aaa1"}, TaskEvidence{
		Task: "lint", Observed: 1, Indeterminate: 1,
		IndeterminateCommits: []string{"ccc3"},
	})
	readiness := ShadowReadiness{
		Threshold:    2,
		Insufficient: []string{"lint"},
		Shortfall:    []TaskShortfall{{Task: "lint", Graded: 0, Remaining: 2}},
	}

	refusal := refusedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "ccc3") || !strings.Contains(refusal, "never judged on") {
		t.Fatalf("refusal does not name the commit on the line it would have printed: %s", refusal)
	}
}

func TestAnUnnamedCommitIsRefusedBeforeTheAccumulation(t *testing.T) {
	// The accumulation carries a blank of its own, so the blank commit
	// would answer to the accumulated set and render a list with a gap in
	// it. The blank is read first for exactly that reason.
	evidence := namedCommitEvidence([]string{"aaa1", ""}, TaskEvidence{
		Task: "build", Observed: 1, Graded: 1, Conflicting: 1,
		ConflictingCommits: []string{"  "},
	})
	readiness := ShadowReadiness{Threshold: 1, Conflicting: []string{"build"}}

	refusal := refusedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "an unnamed commit") {
		t.Fatalf("whitespace was not read as no commit at all: %s", refusal)
	}
	if strings.Contains(refusal, "not one of the accumulated commits") {
		t.Fatalf("a blank commit was answered against the accumulation: %s", refusal)
	}
}

func TestTheConflictingCommitsAreReadBeforeTheInsufficientOnes(t *testing.T) {
	// The sections are printed in that order, so the refusals come in that
	// order too.
	evidence := namedCommitEvidence([]string{"aaa1"},
		TaskEvidence{
			Task: "build", Observed: 1, Graded: 1, Conflicting: 1,
			ConflictingCommits: []string{"bbb2"},
		},
		TaskEvidence{
			Task: "lint", Observed: 1, Indeterminate: 1,
			IndeterminateCommits: []string{"ccc3"},
		},
	)
	readiness := ShadowReadiness{
		Threshold:    2,
		Conflicting:  []string{"build"},
		Insufficient: []string{"lint"},
		Shortfall:    []TaskShortfall{{Task: "lint", Graded: 0, Remaining: 2}},
	}

	refusal := refusedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "bbb2") {
		t.Fatalf("the conflicting line was not read first: %s", refusal)
	}
	if strings.Contains(refusal, "ccc3") {
		t.Fatalf("refused for the later section: %s", refusal)
	}
}

// *** A READY TASK'S COMMIT LISTS ARE DELIBERATELY NOT READ. The page prints
// neither of them for a ready task and no count on the page comes off either,
// so there is no line an operator could have been sent to. Widening this to
// every task later is a re-argument, not a fix. ***
func TestAReadyTaskKeepsCommitListsThePageNeverPrints(t *testing.T) {
	evidence := namedCommitEvidence([]string{"aaa1"}, TaskEvidence{
		Task: "build", Observed: 1, Graded: 1, Agreed: 1,
		ConflictingCommits:   []string{"never-accumulated"},
		IndeterminateCommits: []string{""},
	})
	readiness := ShadowReadiness{Threshold: 1, Ready: []string{"build"}}

	var page strings.Builder
	if err := RenderShadowEvidence(&page, evidence, readiness); err != nil {
		t.Fatalf("refused a page over a list it never prints: %v", err)
	}
	rendered := page.String()
	if !strings.Contains(rendered, "ready (may move)") {
		t.Fatalf("the ready section is missing:\n%s", rendered)
	}
	if strings.Contains(rendered, "never-accumulated") {
		t.Fatalf("a ready task's conflicting commits were printed:\n%s", rendered)
	}
}

func TestACommitThePageNamesAndTheEvidenceAccumulatedIsRendered(t *testing.T) {
	evidence := namedCommitEvidence([]string{"aaa1", "bbb2"}, TaskEvidence{
		Task: "build", Observed: 2, Graded: 1, Conflicting: 1,
		ConflictingCommits: []string{"aaa1", "bbb2"},
	})
	readiness := ShadowReadiness{Threshold: 1, Conflicting: []string{"build"}}

	var page strings.Builder
	if err := RenderShadowEvidence(&page, evidence, readiness); err != nil {
		t.Fatalf("refused an ordinary page: %v", err)
	}
	if !strings.Contains(page.String(), "disagreed on: aaa1, bbb2") {
		t.Fatalf("the commits to go back to are not named:\n%s", page.String())
	}
}

func TestAReadinessForOtherEvidenceIsReadBeforeTheCommitsItWouldName(t *testing.T) {
	// A readiness that does not answer for this evidence would name a task
	// whose counts belong to another; reading its commit lists first would
	// refuse the page for the wrong reason.
	evidence := namedCommitEvidence([]string{"aaa1"}, TaskEvidence{
		Task: "build", Observed: 1, Graded: 1, Conflicting: 1,
		ConflictingCommits: []string{"bbb2"},
	})
	readiness := ShadowReadiness{Threshold: 1, Conflicting: []string{"vet"}}

	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, readiness)
	if !errors.Is(err, ErrShadowEvidenceMismatch) {
		t.Fatalf("refused with %v, want ErrShadowEvidenceMismatch", err)
	}
	if strings.Contains(err.Error(), "bbb2") {
		t.Fatalf("refused for the commit rather than for the readiness: %v", err)
	}
}
