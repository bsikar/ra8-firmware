// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

// An evidence page is read once, by a person deciding whether a required
// check may move. shadow_evidence_render_test.go renders pairs built the way
// a caller builds them, where the readiness is read from the evidence. This
// file takes the other half: a readiness assembled by hand that does not
// answer for the evidence it was handed, and the page that cannot be written
// at all. Every refusal below is a page that would otherwise have read as
// authoritative while counting something nobody accumulated.

// refusingWriter fails on the one write the renderer makes.
type refusingWriter struct{ err error }

func (r refusingWriter) Write([]byte) (int, error) { return 0, r.err }

// The page is assembled whole and written once, so a writer that refuses
// gives back the write's own failure rather than half a page on the
// operator's terminal.
func TestAPageThatCannotBeWrittenIsReportedNotHalfPrinted(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 1,
		TaskEvidence{Task: "build", Observed: 1, Graded: 1, Agreed: 1},
	)

	broken := errors.New("terminal closed")
	err := RenderShadowEvidence(refusingWriter{err: broken}, evidence, readiness)
	if err == nil || !strings.Contains(err.Error(), "write shadow evidence") {
		t.Fatalf("a refusing writer: %v", err)
	}
	if !errors.Is(err, broken) {
		t.Fatalf("the writer's own failure was not carried through: %v", err)
	}
}

// The bound on a rendered commit list is held over the indeterminate commits
// too, not only the conflicting ones. Both lists are printed, so either can
// be the one that turns the page into something nobody will read.
func TestTooManyNeverJudgedCommitsIsRefusedLikeTooManyDisagreements(t *testing.T) {
	commits := make([]string, maxRenderedEvidenceCommits+1)
	for i := range commits {
		commits[i] = strings.Repeat(itoaForTest(i%10), 40)
	}
	// Distinct heads, so the refusal is about how many there are.
	for i := range commits {
		commits[i] = commits[i][:36] + itoaForTest(1000+i)
	}

	evidence, readiness := renderedEvidence(t, len(commits)+1, TaskEvidence{
		Task: "build", Observed: len(commits), Graded: 0,
		Indeterminate: len(commits), IndeterminateCommits: commits,
	})

	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, readiness)
	if !errors.Is(err, ErrShadowEvidenceTooLarge) {
		t.Fatalf("a list past the bound: %v", err)
	}
	if !strings.Contains(err.Error(), "never judged") {
		t.Fatalf("the refusal did not name which list was too long: %v", err)
	}
	if page.Len() != 0 {
		t.Fatalf("a refused page was partly written:\n%s", page.String())
	}
}

// An unnamed task cannot be indexed, answered for, or printed, so it is
// refused before any of that is attempted rather than rendering a blank
// heading with real counts under it.
func TestEvidenceCoveringAnUnnamedTaskIsRefused(t *testing.T) {
	evidence := ShadowEvidence{
		Commits: []string{renderEvidenceCommitA},
		Tasks:   []TaskEvidence{{Task: "", Observed: 1, Graded: 1, Agreed: 1}},
	}
	readiness := ShadowReadiness{Threshold: 1}

	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, readiness)
	if !errors.Is(err, ErrShadowEvidenceReportInvalid) {
		t.Fatalf("an unnamed task: %v", err)
	}
	if !strings.Contains(err.Error(), "unnamed task") {
		t.Fatalf("the refusal did not say what was wrong: %v", err)
	}
	if page.Len() != 0 {
		t.Fatalf("a refused page was partly written:\n%s", page.String())
	}
}

// A task answered for in two lists at once has no single answer, and the
// page would print it under both headings. The readiness is refused by name.
func TestATaskAnsweredForTwiceIsRefusedByName(t *testing.T) {
	evidence, _ := renderedEvidence(t, 1,
		TaskEvidence{Task: "build", Observed: 1, Graded: 1, Agreed: 1},
	)
	doubled := ShadowReadiness{
		Threshold: 1, Ready: []string{"build"}, Conflicting: []string{"build"},
	}

	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, doubled)
	if !errors.Is(err, ErrShadowEvidenceMismatch) {
		t.Fatalf("a task in two lists: %v", err)
	}
	if !strings.Contains(err.Error(), `"build" is answered for twice`) {
		t.Fatalf("the refusal did not name the task: %v", err)
	}
	if page.Len() != 0 {
		t.Fatalf("a refused page was partly written:\n%s", page.String())
	}
}

// The shortfall is indexed by task, so a shortfall naming one task twice
// would print one entry's remaining count and silently drop the other. It is
// refused, and so is a shortfall that covers a task nothing is insufficient
// about.
func TestAShortfallThatDoesNotMatchTheInsufficientTasksIsRefused(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 3, TaskEvidence{
		Task: "build", Observed: 1, Graded: 1, Agreed: 1,
	})
	if len(readiness.Insufficient) != 1 {
		t.Fatalf("the fixture is not short of the threshold: %+v", readiness)
	}

	twice := readiness
	twice.Shortfall = []TaskShortfall{
		{Task: "build", Graded: 1, Remaining: 2},
		{Task: "build", Graded: 1, Remaining: 2},
	}
	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, twice)
	if !errors.Is(err, ErrShadowEvidenceMismatch) ||
		!strings.Contains(err.Error(), `shortfall names "build" twice`) {
		t.Fatalf("a doubled shortfall: %v", err)
	}
	if page.Len() != 0 {
		t.Fatalf("a refused page was partly written:\n%s", page.String())
	}

	extra := readiness
	extra.Shortfall = []TaskShortfall{
		{Task: "build", Graded: 1, Remaining: 2},
		{Task: "lint", Graded: 0, Remaining: 3},
	}
	page.Reset()
	err = RenderShadowEvidence(&page, evidence, extra)
	if !errors.Is(err, ErrShadowEvidenceMismatch) {
		t.Fatalf("a shortfall covering more than is insufficient: %v", err)
	}
	if !strings.Contains(err.Error(), "2 task(s), 1 are insufficient") {
		t.Fatalf("the refusal did not count both sides: %v", err)
	}
	if page.Len() != 0 {
		t.Fatalf("a refused page was partly written:\n%s", page.String())
	}
}
