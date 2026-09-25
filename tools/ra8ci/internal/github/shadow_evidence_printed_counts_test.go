// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

// printedCommits builds a list of one commit repeated, the shape both bounds
// tests use: what is under test here is how many commits a line names, never
// which ones.
func printedCommits(commit string, count int) []string {
	commits := make([]string, 0, count)
	for i := 0; i < count; i++ {
		commits = append(commits, commit)
	}
	return commits
}

// refusedPrintedCounts renders a page that is expected to be refused and
// returns the refusal, failing the test if the page was written instead.
func refusedPrintedCounts(t *testing.T, evidence ShadowEvidence, readiness ShadowReadiness) error {
	t.Helper()
	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, readiness)
	if err == nil {
		t.Fatalf("the page was rendered:\n%s", page.String())
	}
	if page.Len() != 0 {
		t.Fatal("a refused page was written anyway")
	}
	return err
}

// The line says two commits disagreed and names one. An operator holding a
// required check opens every pull request in that list and leaves having read
// half the evidence, with nothing on the page to say there was more.
func TestAConflictingLineNamingFewerCommitsThanItCountsIsRefused(t *testing.T) {
	evidence, readiness := countedEvidence(t)
	at := taskAt(t, evidence, "alpha")
	evidence.Tasks[at].Conflicting = 2
	evidence.Tasks[at].Graded = 2
	evidence.Tasks[at].Observed = 2

	err := refusedPrintedCounts(t, evidence, readiness)

	if !errors.Is(err, ErrShadowEvidenceReportInvalid) {
		t.Fatalf("render: %v, want %v", err, ErrShadowEvidenceReportInvalid)
	}
	if !strings.Contains(err.Error(), `"alpha" disagreed on 2 commit(s) and names 1`) {
		t.Fatalf("the refusal does not state the line: %v", err)
	}
}

// The other printed list, on the other section. "(paired on 2, 1 never
// judged)" over a list naming none of them is a wait nobody can go and look
// at.
func TestAnInsufficientLineNamingNoneOfTheCommitsItCountsIsRefused(t *testing.T) {
	evidence, readiness := countedEvidence(t)
	at := taskAt(t, evidence, "bravo")
	evidence.Tasks[at].IndeterminateCommits = nil

	err := refusedPrintedCounts(t, evidence, readiness)

	if !errors.Is(err, ErrShadowEvidenceReportInvalid) {
		t.Fatalf("render: %v, want %v", err, ErrShadowEvidenceReportInvalid)
	}
	if !strings.Contains(err.Error(), `"bravo" was never judged on 1 commit(s) and names 0`) {
		t.Fatalf("the refusal does not state the line: %v", err)
	}
}

// *** THE BOUND IS READ BEFORE THE COUNT, which is the whole reason the bound
// moved forward: a list too long to read is refused as that rather than as a
// disagreement between a count and a list. ***
func TestTheBoundIsReadBeforeTheCountOnTheSameLine(t *testing.T) {
	commits := printedCommits(renderEvidenceCommitA, maxRenderedEvidenceCommits+1)
	evidence, readiness := renderedEvidence(t, 2,
		TaskEvidence{
			Task: "build", Observed: 5, Graded: 5,
			Agreed: 2, Conflicting: 3, ConflictingCommits: commits,
		},
	)

	err := refusedPrintedCounts(t, evidence, readiness)

	if !errors.Is(err, ErrShadowEvidenceTooLarge) {
		t.Fatalf("render: %v, want %v", err, ErrShadowEvidenceTooLarge)
	}
	if strings.Contains(err.Error(), "and names") {
		t.Fatalf("an over-long list was refused as a miscount: %v", err)
	}
}

// The ungraded line is bounded by the same check now that writeUngradedLine no
// longer bounds it, and it is still refused rather than cut.
func TestAnUngradedLineTooLongIsStillRefusedAsTooLarge(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 1,
		TaskEvidence{Task: "build", Observed: 2, Graded: 2, Agreed: 2},
	)
	evidence.UngradedCommits = printedCommits(renderEvidenceCommitA, maxRenderedEvidenceCommits+1)

	err := refusedPrintedCounts(t, evidence, readiness)

	if !errors.Is(err, ErrShadowEvidenceTooLarge) {
		t.Fatalf("render: %v, want %v", err, ErrShadowEvidenceTooLarge)
	}
}

// The counts are read before the commits they name, so a list that is both
// the wrong length and carrying a commit nobody accumulated is refused as the
// line's own arithmetic: the number beside the list is what a reader weighs
// the list by.
func TestTheCountIsReadBeforeTheCommitsItNames(t *testing.T) {
	evidence, readiness := countedEvidence(t)
	at := taskAt(t, evidence, "alpha")
	evidence.Tasks[at].ConflictingCommits = []string{
		commitOne,
		"4444444444444444444444444444444444444444",
	}

	err := refusedPrintedCounts(t, evidence, readiness)

	if !errors.Is(err, ErrShadowEvidenceReportInvalid) {
		t.Fatalf("render: %v, want %v", err, ErrShadowEvidenceReportInvalid)
	}
	if strings.Contains(err.Error(), "accumulated") {
		t.Fatalf("the count was refused as a name: %v", err)
	}
}

// *** A READY TASK'S LISTS ARE STILL UNREAD, the scope checkNamedCommits
// settled: they are printed nowhere and counted nowhere, so holding them to
// anything would refuse a page over a line no reader is shown. ***
func TestAReadyTasksCommitListsAreNotHeldToItsCounts(t *testing.T) {
	evidence, readiness := countedEvidence(t)
	at := taskAt(t, evidence, "charlie")
	evidence.Tasks[at].ConflictingCommits = []string{commitOne, commitTwo}

	var page strings.Builder
	if err := RenderShadowEvidence(&page, evidence, readiness); err != nil {
		t.Fatalf("a ready task was refused over a list the page never prints: %v", err)
	}
}

// Nothing a real accumulation produces is refused: AccumulateShadowEvidence
// appends to each list in the same branch that increments its counter, so both
// identities hold over evidence it built, and the page still renders whole.
func TestAPageBuiltFromARealAccumulationAlwaysAddsUp(t *testing.T) {
	evidence, readiness := countedEvidence(t)

	var page strings.Builder
	if err := RenderShadowEvidence(&page, evidence, readiness); err != nil {
		t.Fatalf("an ordinary page was refused: %v", err)
	}
	for _, task := range evidence.Tasks {
		if len(task.ConflictingCommits) != task.Conflicting {
			t.Fatalf("%q counts %d conflicts and names %d",
				task.Task, task.Conflicting, len(task.ConflictingCommits))
		}
		if len(task.IndeterminateCommits) != task.Indeterminate {
			t.Fatalf("%q counts %d never judged and names %d",
				task.Task, task.Indeterminate, len(task.IndeterminateCommits))
		}
	}
	if !strings.Contains(page.String(), "disagreed on: ") {
		t.Fatalf("the conflicting line lost its commits:\n%s", page.String())
	}
}
