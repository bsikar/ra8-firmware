// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"strings"
	"testing"
)

// ungradedEvidence builds an evidence value carrying an ungraded line of its
// own, which renderedEvidence and namedCommitEvidence do not: the line this
// check reads is the only one on the page written from a field of
// ShadowEvidence rather than from a task.
func ungradedEvidence(commits, ungradedCommits []string, tasks ...TaskEvidence) ShadowEvidence {
	return ShadowEvidence{Commits: commits, Tasks: tasks, UngradedCommits: ungradedCommits}
}

// A commit that graded nothing moved no task one step closer to its threshold.
// A conflicting task naming it as one it disagreed on is the page saying both,
// four lines apart, about the same commit.
func TestACommitThatGradedNothingAndAlsoConflictedIsRefused(t *testing.T) {
	evidence := ungradedEvidence(
		[]string{"aaa1", "bbb2"},
		[]string{"aaa1"},
		TaskEvidence{
			Task: "build", Observed: 2, Graded: 1, Conflicting: 1, Indeterminate: 1,
			ConflictingCommits: []string{"aaa1"},
		},
	)
	readiness := ShadowReadiness{Threshold: 1, Conflicting: []string{"build"}}

	refusal := refusedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "aaa1") || !strings.Contains(refusal, "build") {
		t.Fatalf("refusal %q names neither the commit nor the task", refusal)
	}
	if !strings.Contains(refusal, "graded nothing") {
		t.Fatalf("refusal %q does not say which line it read", refusal)
	}
}

// *** THE OTHER LIST IS DELIBERATELY NOT READ AGAINST THE UNGRADED LINE. A
// commit that graded nothing came back indeterminate on every task it paired,
// so an insufficient task's "never judged on" list naming it is the two lines
// agreeing. Refusing it would refuse every page that has an ungraded commit and
// an insufficient task, which is the ordinary unclean page. ***
func TestACommitThatGradedNothingIsStillNamedAsNeverJudged(t *testing.T) {
	evidence := ungradedEvidence(
		[]string{"aaa1"},
		[]string{"aaa1"},
		TaskEvidence{
			Task: "build", Observed: 1, Indeterminate: 1,
			IndeterminateCommits: []string{"aaa1"},
		},
	)
	readiness := ShadowReadiness{
		Threshold:    2,
		Insufficient: []string{"build"},
		Shortfall:    []TaskShortfall{{Task: "build", Graded: 0, Remaining: 2}},
	}

	page := renderEvidencePage(t, evidence, readiness)
	if !strings.Contains(page, "graded nothing") || !strings.Contains(page, "never judged on: aaa1") {
		t.Fatalf("page does not carry both lines:\n%s", page)
	}
}

// *** PINNED OPEN, DO NOT "COMPLETE" THIS CHECK BY REFUSING A DUPLICATE HERE.
// The printed count is len(UngradedCommits), so naming one commit twice does
// overstate how much of the evidence counted for nothing. It is not refused in
// this position because the list's size bound is read inside writeUngradedLine
// while the page is written, after every check, and
// TestAPageRefusesMoreUngradedCommitsThanAnyoneReads builds its over-long list
// by repeating one commit: a duplicate check here refuses that page as a
// duplicate rather than as too long to read. #1665 found the same trap on the
// per-task commit lists. ***
func TestACommitNamedTwiceAsUngradedStillRenders(t *testing.T) {
	evidence := ungradedEvidence(
		[]string{"aaa1", "bbb2"},
		[]string{"aaa1", "aaa1"},
		TaskEvidence{Task: "build", Observed: 2, Graded: 2, Agreed: 2},
	)
	readiness := ShadowReadiness{Threshold: 1, Ready: []string{"build"}}

	page := renderEvidencePage(t, evidence, readiness)
	if !strings.Contains(page, "2 of them graded nothing") {
		t.Fatalf("page does not carry the ungraded line:\n%s", page)
	}
}

// A blank renders "2 of them graded nothing: aaa1, ".
func TestAnUnnamedUngradedCommitIsRefused(t *testing.T) {
	evidence := ungradedEvidence(
		[]string{"aaa1", ""},
		[]string{"aaa1", "  "},
		TaskEvidence{Task: "build", Observed: 2, Graded: 1, Agreed: 1, Indeterminate: 1},
	)
	readiness := ShadowReadiness{Threshold: 1, Ready: []string{"build"}}

	refusal := refusedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "unnamed") {
		t.Fatalf("refusal %q does not say the commit is unnamed", refusal)
	}
}

// The blank is read here rather than left to writeUngradedLine's accumulation
// check, which a blank carried in the accumulation would pass.
func TestABlankUngradedCommitTheAccumulationCarriesIsStillRefused(t *testing.T) {
	evidence := ungradedEvidence(
		[]string{"aaa1", ""},
		[]string{""},
		TaskEvidence{Task: "build", Observed: 2, Graded: 1, Agreed: 1, Indeterminate: 1},
	)
	readiness := ShadowReadiness{Threshold: 1, Ready: []string{"build"}}

	refusal := refusedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "unnamed") {
		t.Fatalf("refusal %q does not say the commit is unnamed", refusal)
	}
}

// The task's own list is read first, so a conflicting list that names a commit
// nobody accumulated refuses as that list's fault rather than as a
// contradiction with a line printed above it.
func TestTheConflictingListIsReadBeforeTheUngradedLine(t *testing.T) {
	evidence := ungradedEvidence(
		[]string{"aaa1"},
		[]string{"aaa1"},
		TaskEvidence{
			Task: "build", Observed: 2, Graded: 1, Conflicting: 1, Indeterminate: 1,
			ConflictingCommits: []string{"ccc3"},
		},
	)
	readiness := ShadowReadiness{Threshold: 1, Conflicting: []string{"build"}}

	refusal := refusedEvidencePage(t, evidence, readiness)
	if !strings.Contains(refusal, "not one of the accumulated commits") {
		t.Fatalf("refusal %q is not the accumulation refusal", refusal)
	}
	if strings.Contains(refusal, "graded nothing") {
		t.Fatalf("refusal %q read the ungraded line first", refusal)
	}
}

// Nothing real is refused: AccumulateShadowEvidence puts a commit on
// UngradedCommits only where every pairing on it came back indeterminate, so it
// cannot also be on a task's conflicting list.
func TestARealAccumulationNeverContradictsItsOwnUngradedLine(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, conflicting("build"), agreeing("lint")),
		gradedReport(t, evidenceCommitB, ungraded("build"), ungraded("lint")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	readiness, err := evidence.Readiness(1)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}

	page := renderEvidencePage(t, evidence, readiness)
	if !strings.Contains(page, evidenceCommitB) {
		t.Fatalf("page does not name the commit that graded nothing:\n%s", page)
	}
	if !strings.Contains(page, "disagreed on: "+evidenceCommitA) {
		t.Fatalf("page does not name the commit that conflicted:\n%s", page)
	}
}
