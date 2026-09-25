// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

const (
	renderEvidenceCommitA = "1111111111111111111111111111111111111111"
	renderEvidenceCommitB = "2222222222222222222222222222222222222222"
	renderEvidenceCommitC = "3333333333333333333333333333333333333333"
)

// renderedEvidence is one accumulation and its readiness, built the way a
// caller builds them: the readiness is READ FROM the evidence rather than
// written beside it, so every test below renders a pair that agrees.
func renderedEvidence(t *testing.T, threshold int, tasks ...TaskEvidence) (ShadowEvidence, ShadowReadiness) {
	t.Helper()
	evidence := ShadowEvidence{
		Commits: []string{renderEvidenceCommitA, renderEvidenceCommitB},
		Tasks:   tasks,
	}
	readiness, err := evidence.Readiness(threshold)
	if err != nil {
		t.Fatalf("readiness at %d: %v", threshold, err)
	}
	return evidence, readiness
}

func renderEvidencePage(t *testing.T, evidence ShadowEvidence, readiness ShadowReadiness) string {
	t.Helper()
	var page strings.Builder
	if err := RenderShadowEvidence(&page, evidence, readiness); err != nil {
		t.Fatalf("render shadow evidence: %v", err)
	}
	return page.String()
}

func TestASettledPageSaysEveryTaskMayMove(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 2,
		TaskEvidence{Task: "build", Observed: 2, Graded: 2, Agreed: 2},
		TaskEvidence{Task: "lint", Observed: 2, Graded: 2, Agreed: 1, Divergent: 1},
	)
	page := renderEvidencePage(t, evidence, readiness)

	if !strings.Contains(page, "every compared task is ready") {
		t.Fatalf("a settled page does not state the decision:\n%s", page)
	}
	if strings.Contains(page, "holds the required checks") {
		t.Fatalf("a settled page holds nothing:\n%s", page)
	}
	if !strings.Contains(page, "shadow evidence over 2 commits, threshold 2") {
		t.Fatalf("the page does not say what it was read at:\n%s", page)
	}
	if !strings.Contains(page, "lint: 2 graded (1 agreed, 1 divergent)") {
		t.Fatalf("a ready task is not accounted for:\n%s", page)
	}
}

// The verdict line is the decision, not the counts, and an unclean page has to
// say WHICH work is left: a disagreement is argued about, an insufficiency is
// waited out, and an operator reading only "not ready" cannot tell which.
func TestTheHoldReasonNamesTheWorkThatIsLeft(t *testing.T) {
	for _, testCase := range []struct {
		name  string
		tasks []TaskEvidence
		want  string
	}{
		{
			name: "only a disagreement",
			tasks: []TaskEvidence{
				{Task: "build", Observed: 2, Graded: 2, Agreed: 1, Conflicting: 1,
					ConflictingCommits: []string{renderEvidenceCommitA}},
			},
			want: "holds the required checks: 1 disagreed with Actions\n",
		},
		{
			name: "only a shortfall",
			tasks: []TaskEvidence{
				{Task: "build", Observed: 1, Graded: 1, Agreed: 1},
			},
			want: "holds the required checks: 1 short of the threshold\n",
		},
		{
			name: "both",
			tasks: []TaskEvidence{
				{Task: "build", Observed: 2, Graded: 2, Agreed: 1, Conflicting: 1,
					ConflictingCommits: []string{renderEvidenceCommitA}},
				{Task: "lint", Observed: 1, Graded: 1, Agreed: 1},
			},
			want: "holds the required checks: 1 disagreed with Actions, 1 short of the threshold\n",
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			evidence, readiness := renderedEvidence(t, 2, testCase.tasks...)
			page := renderEvidencePage(t, evidence, readiness)
			if !strings.Contains(page, testCase.want) {
				t.Fatalf("want %q on the page:\n%s", testCase.want, page)
			}
		})
	}
}

// The sections are in the order the decision is made in, not alphabetical
// order. A page that opened with the tasks that are fine would bury the reason
// the required checks are still where they are.
func TestTheSectionsAreInDecisionOrder(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 2,
		TaskEvidence{Task: "aaa-ready", Observed: 2, Graded: 2, Agreed: 2},
		TaskEvidence{Task: "mmm-short", Observed: 1, Graded: 1, Agreed: 1},
		TaskEvidence{Task: "zzz-conflicting", Observed: 2, Graded: 2, Agreed: 1, Conflicting: 1,
			ConflictingCommits: []string{renderEvidenceCommitB}},
	)
	page := renderEvidencePage(t, evidence, readiness)

	conflicting := strings.Index(page, "conflicting (ra8ci and Actions disagreed")
	insufficient := strings.Index(page, "insufficient (no disagreement")
	ready := strings.Index(page, "ready (may move)")
	if conflicting < 0 || insufficient < 0 || ready < 0 {
		t.Fatalf("a section is missing:\n%s", page)
	}
	if !(conflicting < insufficient && insufficient < ready) {
		t.Fatalf("sections are not in decision order (%d, %d, %d):\n%s",
			conflicting, insufficient, ready, page)
	}
}

// Naming the commits is the whole point of ConflictingCommits and
// IndeterminateCommits: a task held short moves forward by going back to
// exactly those pull requests, and a page that only counted them would send an
// operator back through the accumulation to work out which they were.
func TestTheCommitsToGoBackToAreNamed(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 3,
		TaskEvidence{Task: "build", Observed: 2, Graded: 2, Agreed: 1, Conflicting: 1,
			ConflictingCommits: []string{renderEvidenceCommitB}},
		TaskEvidence{Task: "lint", Observed: 3, Graded: 1, Agreed: 1, Indeterminate: 2,
			IndeterminateCommits: []string{renderEvidenceCommitC, renderEvidenceCommitA}},
	)
	page := renderEvidencePage(t, evidence, readiness)

	if !strings.Contains(page, "disagreed on: "+renderEvidenceCommitB) {
		t.Fatalf("the conflicting commit is not named:\n%s", page)
	}
	// In the order the reports were given, not sorted: that order is the
	// caller's record of what happened.
	if !strings.Contains(page, "never judged on: "+renderEvidenceCommitC+", "+renderEvidenceCommitA) {
		t.Fatalf("the unjudged commits are not named in report order:\n%s", page)
	}
}

// A shortfall on the page is the difference between waiting for the next merge
// and going looking for pull requests that exercise the task.
func TestThePageSaysHowFarShortAnInsufficientTaskIs(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 5,
		TaskEvidence{Task: "build", Observed: 4, Graded: 2, Agreed: 2, Indeterminate: 2,
			IndeterminateCommits: []string{renderEvidenceCommitA, renderEvidenceCommitB}},
	)
	page := renderEvidencePage(t, evidence, readiness)

	if !strings.Contains(page, "build: graded on 2, 3 more needed (paired on 4, 2 never judged)") {
		t.Fatalf("the shortfall is not stated:\n%s", page)
	}
}

// A conflicting task carries no shortfall, and the page must not invent one:
// "grade this many more and it is ready" is false of a conflict, which is the
// call Readiness already makes and this page has to keep.
func TestAConflictingTaskIsNeverGivenARemainingCount(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 4,
		TaskEvidence{Task: "build", Observed: 2, Graded: 2, Agreed: 1, Conflicting: 1,
			ConflictingCommits: []string{renderEvidenceCommitA}},
	)
	page := renderEvidencePage(t, evidence, readiness)

	if strings.Contains(page, "more needed") {
		t.Fatalf("a conflict was given a remaining count:\n%s", page)
	}
	if !strings.Contains(page, "build: 1 of 2 graded commits disagreed") {
		t.Fatalf("the disagreement is not stated:\n%s", page)
	}
}

// The two arguments are separate values, so a caller can hand over a readiness
// read from some other accumulation. Rendering it would print one task's
// commits under another task's verdict.
func TestAReadinessForOtherEvidenceIsRefused(t *testing.T) {
	evidence, _ := renderedEvidence(t, 2,
		TaskEvidence{Task: "build", Observed: 2, Graded: 2, Agreed: 2},
	)
	other := ShadowEvidence{
		Commits: []string{renderEvidenceCommitA},
		Tasks:   []TaskEvidence{{Task: "lint", Observed: 2, Graded: 2, Agreed: 2}},
	}
	readiness, err := other.Readiness(2)
	if err != nil {
		t.Fatalf("readiness: %v", err)
	}
	var page strings.Builder
	err = RenderShadowEvidence(&page, evidence, readiness)
	if !errors.Is(err, ErrShadowEvidenceMismatch) {
		t.Fatalf("want ErrShadowEvidenceMismatch, got %v", err)
	}
	if page.Len() != 0 {
		t.Fatalf("a refused page was written anyway:\n%s", page.String())
	}
}

// A covered task the readiness says nothing about would be left off a page
// whose whole purpose is to say where every task stands, and the refusal names
// it rather than only reporting that the counts differed.
func TestACoveredTaskMissingFromTheAnswerIsRefusedByName(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 2,
		TaskEvidence{Task: "build", Observed: 2, Graded: 2, Agreed: 2},
		TaskEvidence{Task: "lint", Observed: 2, Graded: 2, Agreed: 2},
	)
	readiness.Ready = []string{"build"}

	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, readiness)
	if !errors.Is(err, ErrShadowEvidenceMismatch) {
		t.Fatalf("want ErrShadowEvidenceMismatch, got %v", err)
	}
	if err == nil || !strings.Contains(err.Error(), "lint") {
		t.Fatalf("the refusal does not name the unanswered task: %v", err)
	}
}

// An insufficient name with no shortfall entry beside it is the hand-assembled
// half of the same mistake: the page would print a remaining count of zero for
// a task that is not ready.
func TestAnInsufficientTaskWithNoShortfallIsRefused(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 3,
		TaskEvidence{Task: "build", Observed: 1, Graded: 1, Agreed: 1},
	)
	readiness.Shortfall = nil

	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, readiness)
	if !errors.Is(err, ErrShadowEvidenceMismatch) {
		t.Fatalf("want ErrShadowEvidenceMismatch, got %v", err)
	}
}

func TestAnEmptyAccumulationRendersNothing(t *testing.T) {
	for _, testCase := range []struct {
		name     string
		evidence ShadowEvidence
	}{
		{name: "no commits", evidence: ShadowEvidence{
			Tasks: []TaskEvidence{{Task: "build", Observed: 1, Graded: 1, Agreed: 1}},
		}},
		{name: "no tasks", evidence: ShadowEvidence{
			Commits: []string{renderEvidenceCommitA},
		}},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			var page strings.Builder
			err := RenderShadowEvidence(&page, testCase.evidence, ShadowReadiness{Threshold: 1})
			if !errors.Is(err, ErrShadowEvidenceEmpty) {
				t.Fatalf("want ErrShadowEvidenceEmpty, got %v", err)
			}
		})
	}
}

// A threshold below one is not a readiness answer, the rule Readiness itself
// applies. A page carrying "threshold 0" would read as an answer to a question
// nobody asked.
func TestAThresholdBelowOneIsRefused(t *testing.T) {
	evidence := ShadowEvidence{
		Commits: []string{renderEvidenceCommitA},
		Tasks:   []TaskEvidence{{Task: "build", Observed: 1, Graded: 1, Agreed: 1}},
	}
	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, ShadowReadiness{
		Threshold: 0,
		Ready:     []string{"build"},
	})
	if !errors.Is(err, ErrShadowEvidenceThresholdInvalid) {
		t.Fatalf("want ErrShadowEvidenceThresholdInvalid, got %v", err)
	}
}

// Refusal, never truncation: a page cut short in the middle of its conflicts is
// worse than no page, the argument maxRenderedComparisons already makes for one
// commit.
func TestTooMuchToRenderIsRefusedNotCut(t *testing.T) {
	t.Run("tasks", func(t *testing.T) {
		tasks := make([]TaskEvidence, 0, maxRenderedEvidenceTasks+1)
		names := make([]string, 0, maxRenderedEvidenceTasks+1)
		for i := 0; i <= maxRenderedEvidenceTasks; i++ {
			name := "task-" + strings.Repeat("x", i%3) + string(rune('a'+i%26)) + itoaForTest(i)
			tasks = append(tasks, TaskEvidence{Task: name, Observed: 1, Graded: 1, Agreed: 1})
			names = append(names, name)
		}
		evidence := ShadowEvidence{Commits: []string{renderEvidenceCommitA}, Tasks: tasks}
		var page strings.Builder
		err := RenderShadowEvidence(&page, evidence, ShadowReadiness{Threshold: 1, Ready: names})
		if !errors.Is(err, ErrShadowEvidenceTooLarge) {
			t.Fatalf("want ErrShadowEvidenceTooLarge, got %v", err)
		}
		if page.Len() != 0 {
			t.Fatal("a refused page was written anyway")
		}
	})
	t.Run("commits on one task", func(t *testing.T) {
		commits := make([]string, 0, maxRenderedEvidenceCommits+1)
		for i := 0; i <= maxRenderedEvidenceCommits; i++ {
			commits = append(commits, renderEvidenceCommitA)
		}
		evidence, readiness := renderedEvidence(t, 2,
			TaskEvidence{
				Task: "build", Observed: len(commits), Graded: len(commits),
				Agreed: 1, Conflicting: len(commits) - 1, ConflictingCommits: commits,
			},
		)
		var page strings.Builder
		err := RenderShadowEvidence(&page, evidence, readiness)
		if !errors.Is(err, ErrShadowEvidenceTooLarge) {
			t.Fatalf("want ErrShadowEvidenceTooLarge, got %v", err)
		}
		if page.Len() != 0 {
			t.Fatal("a refused page was written anyway")
		}
	})
}

// The bound is a ceiling, not a lying point: exactly the limit renders whole.
func TestExactlyTheBoundRendersWhole(t *testing.T) {
	commits := make([]string, 0, maxRenderedEvidenceCommits)
	for i := 0; i < maxRenderedEvidenceCommits; i++ {
		commits = append(commits, renderEvidenceCommitA)
	}
	evidence, readiness := renderedEvidence(t, 2,
		TaskEvidence{
			Task: "build", Observed: len(commits), Graded: len(commits),
			Agreed: 1, Conflicting: len(commits) - 1, ConflictingCommits: commits,
		},
	)
	page := renderEvidencePage(t, evidence, readiness)
	if !strings.Contains(page, "disagreed on: ") {
		t.Fatalf("the commit line is missing at exactly the bound:\n%s", page[:min(len(page), 400)])
	}
}

// A task named twice cannot come out of AccumulateShadowEvidence, so an
// evidence value carrying one was assembled by hand; picking a winner would
// render counts nobody accumulated.
func TestATaskCoveredTwiceIsRefused(t *testing.T) {
	evidence := ShadowEvidence{
		Commits: []string{renderEvidenceCommitA},
		Tasks: []TaskEvidence{
			{Task: "build", Observed: 1, Graded: 1, Agreed: 1},
			{Task: "build", Observed: 1, Graded: 1, Agreed: 1},
		},
	}
	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, ShadowReadiness{Threshold: 1, Ready: []string{"build"}})
	if !errors.Is(err, ErrShadowSetAmbiguous) {
		t.Fatalf("want ErrShadowSetAmbiguous, got %v", err)
	}
}

func TestNoWriterIsRefused(t *testing.T) {
	evidence, readiness := renderedEvidence(t, 1,
		TaskEvidence{Task: "build", Observed: 1, Graded: 1, Agreed: 1},
	)
	if err := RenderShadowEvidence(nil, evidence, readiness); err == nil {
		t.Fatal("rendering to no writer was accepted")
	}
}

// A page that says "1 commits" reads as generated rather than written, and this
// one is asking a person to decide something.
func TestOneOfSomethingIsSingular(t *testing.T) {
	evidence := ShadowEvidence{
		Commits: []string{renderEvidenceCommitA},
		Tasks:   []TaskEvidence{{Task: "build", Observed: 1, Graded: 1, Agreed: 1}},
	}
	readiness, err := evidence.Readiness(1)
	if err != nil {
		t.Fatalf("readiness: %v", err)
	}
	page := renderEvidencePage(t, evidence, readiness)
	if !strings.Contains(page, "shadow evidence over 1 commit, threshold 1") {
		t.Fatalf("the count is not singular:\n%s", page)
	}
	if strings.Contains(page, "1 commits") {
		t.Fatalf("a plural crept in:\n%s", page)
	}
}

// itoaForTest keeps the bound test's names unique without pulling strconv into
// this file for one call.
func itoaForTest(n int) string {
	if n == 0 {
		return "0"
	}
	var digits []byte
	for n > 0 {
		digits = append([]byte{byte('0' + n%10)}, digits...)
		n /= 10
	}
	return string(digits)
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
