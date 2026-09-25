// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

const (
	evidenceCommitA = "1111111111111111111111111111111111111111"
	evidenceCommitB = "2222222222222222222222222222222222222222"
	evidenceCommitC = "3333333333333333333333333333333333333333"
)

// gradedReport builds a report the way CompareShadowRun would, so the
// accumulation is exercised against real graded comparisons rather than
// hand-set verdicts.
func gradedReport(t *testing.T, head string, observations ...ShadowObservation) ShadowReport {
	t.Helper()
	for i := range observations {
		observations[i].HeadSHA = head
		if observations[i].ActionsJob == "" {
			observations[i].ActionsJob = "build"
		}
	}
	report, err := CompareShadowRun(observations)
	if err != nil {
		t.Fatalf("CompareShadowRun: %v", err)
	}
	return report
}

func agreeing(task string) ShadowObservation {
	return ShadowObservation{Task: task, Observed: "success", ActionsConclusion: "success"}
}

func diverging(task string) ShadowObservation {
	return ShadowObservation{Task: task, Observed: "failure", ActionsConclusion: "timed_out"}
}

func conflicting(task string) ShadowObservation {
	return ShadowObservation{Task: task, Observed: "failure", ActionsConclusion: "success"}
}

func ungraded(task string) ShadowObservation {
	return ShadowObservation{Task: task, Observed: "success", ActionsConclusion: ""}
}

func taskEvidence(t *testing.T, evidence ShadowEvidence, name string) TaskEvidence {
	t.Helper()
	for _, task := range evidence.Tasks {
		if task.Task == name {
			return task
		}
	}
	t.Fatalf("no evidence for task %q", name)
	return TaskEvidence{}
}

// The evidence #1481 defers the required-check move on is per task, because
// branch protection requires a context per task. A task compared on one pull
// request must not read as compared on three because other tasks were.
func TestEvidenceIsCountedPerTaskNotPerCommit(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build"), agreeing("lint")),
		gradedReport(t, evidenceCommitB, agreeing("build"), agreeing("lint")),
		gradedReport(t, evidenceCommitC, agreeing("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	if got := taskEvidence(t, evidence, "build").Graded; got != 3 {
		t.Fatalf("build graded on %d commits, want 3", got)
	}
	if got := taskEvidence(t, evidence, "lint").Graded; got != 2 {
		t.Fatalf("lint graded on %d commits, want 2", got)
	}
	readiness, err := evidence.Readiness(3)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	if len(readiness.Ready) != 1 || readiness.Ready[0] != "build" {
		t.Fatalf("ready %v, want only build", readiness.Ready)
	}
	if len(readiness.Insufficient) != 1 || readiness.Insufficient[0] != "lint" {
		t.Fatalf("insufficient %v, want only lint", readiness.Insufficient)
	}
	if readiness.Settled() {
		t.Fatal("readiness settled while a covered task is short of the threshold")
	}
}

// Re-grading one pull request produces the same observations again. Counting
// them twice would inflate the evidence a threshold is read against without
// anyone having looked at a second pull request.
func TestOneCommitTwiceIsRefusedRatherThanCountedTwice(t *testing.T) {
	report := gradedReport(t, evidenceCommitA, agreeing("build"))
	_, err := AccumulateShadowEvidence([]ShadowReport{report, report})
	if !errors.Is(err, ErrShadowEvidenceRepeatedCommit) {
		t.Fatalf("error %v, want ErrShadowEvidenceRepeatedCommit", err)
	}
	if !strings.Contains(err.Error(), evidenceCommitA) {
		t.Fatalf("refusal %q does not name the repeated commit", err)
	}
}

func TestAccumulateRefusesReportsItCannotRead(t *testing.T) {
	good := gradedReport(t, evidenceCommitA, agreeing("build"))
	foreign := gradedReport(t, evidenceCommitB, agreeing("build"))
	mixed := good
	mixed.Comparisons = append(append([]ShadowComparison{}, good.Comparisons...), foreign.Comparisons...)

	unnamed := good
	unnamed.Comparisons = []ShadowComparison{{Task: "", HeadSHA: evidenceCommitA, Verdict: ShadowAgreed}}

	repeatedTask := good
	repeatedTask.Comparisons = []ShadowComparison{
		{Task: "build", HeadSHA: evidenceCommitA, Verdict: ShadowAgreed},
		{Task: "build", HeadSHA: evidenceCommitA, Verdict: ShadowConflicting},
	}

	noHead := good
	noHead.HeadSHA = ""

	empty := good
	empty.Comparisons = nil

	for _, testCase := range []struct {
		name   string
		report ShadowReport
		want   error
	}{
		{"no head commit", noHead, ErrShadowEvidenceReportInvalid},
		{"head is not a commit", ShadowReport{HeadSHA: "main", Comparisons: good.Comparisons}, ErrShadowEvidenceReportInvalid},
		{"compares nothing", empty, ErrShadowEvidenceReportInvalid},
		{"carries another commit's comparison", mixed, ErrShadowEvidenceReportInvalid},
		{"unnamed task", unnamed, ErrShadowEvidenceReportInvalid},
		{"one task twice", repeatedTask, ErrShadowSetAmbiguous},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			evidence, err := AccumulateShadowEvidence([]ShadowReport{testCase.report})
			if !errors.Is(err, testCase.want) {
				t.Fatalf("error %v, want %v", err, testCase.want)
			}
			if len(evidence.Tasks) != 0 || len(evidence.Commits) != 0 {
				t.Fatalf("a refused accumulation returned evidence: %+v", evidence)
			}
		})
	}
}

func TestAccumulateRefusesNothingToAccumulate(t *testing.T) {
	if _, err := AccumulateShadowEvidence(nil); !errors.Is(err, ErrShadowEvidenceEmpty) {
		t.Fatalf("error %v, want ErrShadowEvidenceEmpty", err)
	}
}

// shadow_compare.go makes ShadowIndeterminate the zero value so a pairing
// nobody judged cannot read as one that passed. That rule has to survive
// accumulation, or a task compared three times against jobs that never
// completed would read as three pull requests of evidence.
func TestAnUngradedPairingIsObservedButNotGraded(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, ungraded("build")),
		gradedReport(t, evidenceCommitB, ungraded("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	build := taskEvidence(t, evidence, "build")
	if build.Observed != 2 {
		t.Fatalf("observed %d, want 2", build.Observed)
	}
	if build.Graded != 0 || build.Indeterminate != 2 {
		t.Fatalf("graded %d indeterminate %d, want 0 and 2", build.Graded, build.Indeterminate)
	}
	readiness, err := evidence.Readiness(1)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	if len(readiness.Ready) != 0 {
		t.Fatalf("ready %v on pairings nothing judged", readiness.Ready)
	}
	if len(readiness.Insufficient) != 1 || readiness.Insufficient[0] != "build" {
		t.Fatalf("insufficient %v, want only build", readiness.Insufficient)
	}
}

// A divergence is a vocabulary difference branch protection cannot see.
// ShadowReport.Clean() already treats it as clean, so withholding it as
// evidence here would hold a gate on something that changes no outcome.
func TestADivergentPairingCountsAsEvidence(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, diverging("build")),
		gradedReport(t, evidenceCommitB, agreeing("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	build := taskEvidence(t, evidence, "build")
	if build.Graded != 2 || build.Divergent != 1 || build.Agreed != 1 {
		t.Fatalf("graded %d divergent %d agreed %d, want 2, 1, 1", build.Graded, build.Divergent, build.Agreed)
	}
	readiness, err := evidence.Readiness(2)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	if !readiness.Settled() {
		t.Fatalf("readiness not settled: %+v", readiness)
	}
}

// The question is whether ra8ci has ever disagreed with Actions about whether
// a pull request may merge, not what share of the time it agreed. A ratio
// would let one unexplained conflict be outvoted by routine passes.
func TestOneConflictHoldsTheTaskHoweverManyAgreementsFollow(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, conflicting("build")),
		gradedReport(t, evidenceCommitB, agreeing("build")),
		gradedReport(t, evidenceCommitC, agreeing("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	build := taskEvidence(t, evidence, "build")
	if build.Graded != 3 {
		t.Fatalf("graded %d, want 3", build.Graded)
	}
	if len(build.ConflictingCommits) != 1 || build.ConflictingCommits[0] != evidenceCommitA {
		t.Fatalf("conflicting commits %v, want only %s", build.ConflictingCommits, evidenceCommitA)
	}
	readiness, err := evidence.Readiness(1)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	if len(readiness.Conflicting) != 1 || readiness.Conflicting[0] != "build" {
		t.Fatalf("conflicting %v, want only build", readiness.Conflicting)
	}
	if len(readiness.Ready) != 0 {
		t.Fatalf("a task with an unexplained conflict is ready: %v", readiness.Ready)
	}
	if readiness.Settled() {
		t.Fatal("readiness settled with a conflict outstanding")
	}
}

// A conflicting task and an under-observed one need different work: a
// disagreement to explain, not more pull requests to wait for. Every covered
// task lands in exactly one list, so the answer can be read as a partition.
func TestReadinessPlacesEveryCoveredTaskInExactlyOneList(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build"), conflicting("flash"), agreeing("lint")),
		gradedReport(t, evidenceCommitB, agreeing("build"), agreeing("flash"), ungraded("lint")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	readiness, err := evidence.Readiness(2)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	seen := map[string]int{}
	for _, list := range [][]string{readiness.Ready, readiness.Conflicting, readiness.Insufficient} {
		for _, task := range list {
			seen[task]++
		}
	}
	for _, task := range []string{"build", "flash", "lint"} {
		if seen[task] != 1 {
			t.Fatalf("task %q appears %d times across the three lists, want 1", task, seen[task])
		}
	}
	if len(seen) != len(evidence.Tasks) {
		t.Fatalf("%d tasks reported, evidence covers %d", len(seen), len(evidence.Tasks))
	}
	if len(readiness.Ready) != 1 || readiness.Ready[0] != "build" {
		t.Fatalf("ready %v, want only build", readiness.Ready)
	}
	if len(readiness.Conflicting) != 1 || readiness.Conflicting[0] != "flash" {
		t.Fatalf("conflicting %v, want only flash", readiness.Conflicting)
	}
	if len(readiness.Insufficient) != 1 || readiness.Insufficient[0] != "lint" {
		t.Fatalf("insufficient %v, want only lint", readiness.Insufficient)
	}
}

// The threshold is the operator's judgement, kept with the answer because the
// same evidence answers differently at a different one.
func TestTheSameEvidenceAnswersDifferentlyAtDifferentThresholds(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build")),
		gradedReport(t, evidenceCommitB, agreeing("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	at2, err := evidence.Readiness(2)
	if err != nil {
		t.Fatalf("Readiness(2): %v", err)
	}
	if at2.Threshold != 2 || !at2.Settled() {
		t.Fatalf("at threshold 2: %+v", at2)
	}
	at3, err := evidence.Readiness(3)
	if err != nil {
		t.Fatalf("Readiness(3): %v", err)
	}
	if at3.Threshold != 3 || at3.Settled() {
		t.Fatalf("at threshold 3: %+v", at3)
	}
}

func TestReadinessRefusesAThresholdBelowOne(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	for _, threshold := range []int{0, -1} {
		readiness, err := evidence.Readiness(threshold)
		if !errors.Is(err, ErrShadowEvidenceThresholdInvalid) {
			t.Fatalf("threshold %d: error %v, want ErrShadowEvidenceThresholdInvalid", threshold, err)
		}
		if len(readiness.Ready) != 0 || len(readiness.Conflicting) != 0 || len(readiness.Insufficient) != 0 {
			t.Fatalf("threshold %d: a refused answer carried tasks: %+v", threshold, readiness)
		}
	}
}

// Tasks are sorted so two passes diff. Commits keep the order they were given,
// because the order pull requests were observed in is the caller's record and
// sorting by SHA would invent one.
func TestTasksAreOrderedByNameAndCommitsKeepTheOrderGiven(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitC, agreeing("lint"), agreeing("build")),
		gradedReport(t, evidenceCommitA, agreeing("flash")),
		gradedReport(t, evidenceCommitB, agreeing("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	var names []string
	for _, task := range evidence.Tasks {
		names = append(names, task.Task)
	}
	if strings.Join(names, ",") != "build,flash,lint" {
		t.Fatalf("tasks %v, want build,flash,lint", names)
	}
	want := []string{evidenceCommitC, evidenceCommitA, evidenceCommitB}
	if strings.Join(evidence.Commits, ",") != strings.Join(want, ",") {
		t.Fatalf("commits %v, want %v", evidence.Commits, want)
	}
}

// This answers for the evidence in hand. A catalog task no pull request
// exercised is not silently declared ready, and is not invented into the
// answer either: `ra8ci github required-checks` already names the tasks the
// declared correspondence does not cover.
func TestReadinessNeverNamesATaskTheEvidenceDoesNotCover(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	readiness, err := evidence.Readiness(1)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	for _, list := range [][]string{readiness.Ready, readiness.Conflicting, readiness.Insufficient} {
		for _, task := range list {
			if task != "build" {
				t.Fatalf("readiness names %q, which no accumulated report mentions", task)
			}
		}
	}
	if !readiness.Settled() {
		t.Fatalf("readiness not settled: %+v", readiness)
	}
}

// Settled is an answer about covered tasks, so evidence covering nothing is
// never settled. Accumulation refuses an empty set, but a zero value can reach
// Readiness through a caller that ignored the error.
func TestEmptyEvidenceIsNeverSettled(t *testing.T) {
	readiness, err := ShadowEvidence{}.Readiness(1)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	if readiness.Settled() {
		t.Fatal("empty evidence reported as settled")
	}
}

// TestTheCommitsNobodyJudgedAreNamedNotJustCounted pins the point of this
// slice: a task short of its threshold is chased through the pull requests
// whose Actions side stated no outcome, and a count does not say which.
func TestTheCommitsNobodyJudgedAreNamedNotJustCounted(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, ungraded("build")),
		gradedReport(t, evidenceCommitB, agreeing("build")),
		gradedReport(t, evidenceCommitC, ungraded("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	build := taskEvidence(t, evidence, "build")
	if build.Indeterminate != 2 {
		t.Fatalf("indeterminate = %d, want 2", build.Indeterminate)
	}
	want := []string{evidenceCommitA, evidenceCommitC}
	if len(build.IndeterminateCommits) != len(want) {
		t.Fatalf("indeterminate commits = %v, want %v", build.IndeterminateCommits, want)
	}
	for i, commit := range want {
		if build.IndeterminateCommits[i] != commit {
			t.Fatalf("indeterminate commit %d = %s, want %s", i, build.IndeterminateCommits[i], commit)
		}
	}
	if len(build.IndeterminateCommits) != build.Indeterminate {
		t.Fatalf("named %d ungraded commits for a count of %d",
			len(build.IndeterminateCommits), build.Indeterminate)
	}
}

// TestUngradedCommitsKeepTheOrderTheReportsWereGiven holds them to the
// ConflictingCommits convention: the order pull requests were observed in is
// the caller's record of what happened.
func TestUngradedCommitsKeepTheOrderTheReportsWereGiven(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitC, ungraded("build")),
		gradedReport(t, evidenceCommitA, ungraded("build")),
		gradedReport(t, evidenceCommitB, ungraded("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	build := taskEvidence(t, evidence, "build")
	want := []string{evidenceCommitC, evidenceCommitA, evidenceCommitB}
	for i, commit := range want {
		if build.IndeterminateCommits[i] != commit {
			t.Fatalf("ungraded commit %d = %s, want %s", i, build.IndeterminateCommits[i], commit)
		}
	}
}

// TestAGradedCommitIsNeverNamedAsUngraded pins that the two lists do not
// overlap: agreement, divergence and conflict all produced a verdict.
func TestAGradedCommitIsNeverNamedAsUngraded(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build")),
		gradedReport(t, evidenceCommitB, diverging("build")),
		gradedReport(t, evidenceCommitC, conflicting("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	build := taskEvidence(t, evidence, "build")
	if len(build.IndeterminateCommits) != 0 {
		t.Fatalf("graded commits named as ungraded: %v", build.IndeterminateCommits)
	}
}

// TestAConflictingTaskStillNamesItsUnjudgedCommits pins that the two lists are
// about different things. A conflict is work of its own; a pairing nobody
// judged is still unjudged beside it.
func TestAConflictingTaskStillNamesItsUnjudgedCommits(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, conflicting("build")),
		gradedReport(t, evidenceCommitB, ungraded("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	build := taskEvidence(t, evidence, "build")
	if len(build.ConflictingCommits) != 1 || build.ConflictingCommits[0] != evidenceCommitA {
		t.Fatalf("conflicting commits = %v, want [%s]", build.ConflictingCommits, evidenceCommitA)
	}
	if len(build.IndeterminateCommits) != 1 || build.IndeterminateCommits[0] != evidenceCommitB {
		t.Fatalf("ungraded commits = %v, want [%s]", build.IndeterminateCommits, evidenceCommitB)
	}
}

// TestAnInsufficientTaskSaysHowFarShortItIs pins the shortfall: a name on its
// own does not say whether one more pull request finishes the task.
func TestAnInsufficientTaskSaysHowFarShortItIs(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build"), ungraded("lint")),
		gradedReport(t, evidenceCommitB, agreeing("build"), ungraded("lint")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	readiness, err := evidence.Readiness(3)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	if len(readiness.Shortfall) != len(readiness.Insufficient) {
		t.Fatalf("%d shortfalls for %d insufficient tasks",
			len(readiness.Shortfall), len(readiness.Insufficient))
	}
	shortfall := map[string]TaskShortfall{}
	for _, task := range readiness.Shortfall {
		shortfall[task.Task] = task
	}
	build, ok := shortfall["build"]
	if !ok {
		t.Fatalf("no shortfall for build: %+v", readiness.Shortfall)
	}
	if build.Graded != 2 || build.Remaining != 1 {
		t.Fatalf("build shortfall = %+v, want graded 2 remaining 1", build)
	}
	lint, ok := shortfall["lint"]
	if !ok {
		t.Fatalf("no shortfall for lint: %+v", readiness.Shortfall)
	}
	if lint.Graded != 0 || lint.Remaining != 3 {
		t.Fatalf("lint shortfall = %+v, want graded 0 remaining 3", lint)
	}
}

// TestTheShortfallCoversTheInsufficientTasksAndNothingElse pins the judgement
// call: a conflicting task carries no remaining count, because no number of
// pull requests clears a conflict, and a ready task has nothing to report.
func TestTheShortfallCoversTheInsufficientTasksAndNothingElse(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build"), conflicting("lint"), ungraded("docs")),
		gradedReport(t, evidenceCommitB, agreeing("build"), agreeing("lint"), ungraded("docs")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	readiness, err := evidence.Readiness(2)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	if len(readiness.Ready) != 1 || readiness.Ready[0] != "build" {
		t.Fatalf("ready = %v, want [build]", readiness.Ready)
	}
	if len(readiness.Conflicting) != 1 || readiness.Conflicting[0] != "lint" {
		t.Fatalf("conflicting = %v, want [lint]", readiness.Conflicting)
	}
	if len(readiness.Shortfall) != 1 {
		t.Fatalf("shortfall = %+v, want one entry", readiness.Shortfall)
	}
	if readiness.Shortfall[0].Task != "docs" {
		t.Fatalf("shortfall names %q, want docs", readiness.Shortfall[0].Task)
	}
	if readiness.Shortfall[0].Remaining != 2 {
		t.Fatalf("docs remaining = %d, want 2", readiness.Shortfall[0].Remaining)
	}
}

// TestTheShortfallIsInTheSameOrderAsTheInsufficientList pins that the two can
// be read side by side.
func TestTheShortfallIsInTheSameOrderAsTheInsufficientList(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, ungraded("zeta"), ungraded("alpha"), ungraded("mid")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	readiness, err := evidence.Readiness(1)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	if len(readiness.Insufficient) != len(readiness.Shortfall) {
		t.Fatalf("%d insufficient, %d shortfalls", len(readiness.Insufficient), len(readiness.Shortfall))
	}
	for i, name := range readiness.Insufficient {
		if readiness.Shortfall[i].Task != name {
			t.Fatalf("shortfall %d is %q beside insufficient %q", i, readiness.Shortfall[i].Task, name)
		}
	}
}

// TestARemainingCountIsNeverZeroOrBelow pins that the shortfall is only ever
// written for a task that really is short: a task graded past its threshold is
// ready, not a shortfall of zero or a negative one.
func TestARemainingCountIsNeverZeroOrBelow(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build"), ungraded("lint")),
		gradedReport(t, evidenceCommitB, agreeing("build"), ungraded("lint")),
		gradedReport(t, evidenceCommitC, agreeing("build"), ungraded("lint")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	for _, threshold := range []int{1, 2, 3} {
		readiness, err := evidence.Readiness(threshold)
		if err != nil {
			t.Fatalf("Readiness(%d): %v", threshold, err)
		}
		for _, task := range readiness.Shortfall {
			if task.Remaining < 1 {
				t.Fatalf("threshold %d: %q remaining %d", threshold, task.Task, task.Remaining)
			}
			if task.Graded+task.Remaining != threshold {
				t.Fatalf("threshold %d: %q graded %d remaining %d do not add up",
					threshold, task.Task, task.Graded, task.Remaining)
			}
		}
	}
}

// TestSettledEvidenceReportsNoShortfall pins that a settled answer carries an
// empty shortfall rather than a nil one a reader has to guess at.
func TestSettledEvidenceReportsNoShortfall(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	readiness, err := evidence.Readiness(1)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	if !readiness.Settled() {
		t.Fatalf("readiness is not settled: %+v", readiness)
	}
	if readiness.Shortfall == nil {
		t.Fatal("shortfall is nil, want an empty list")
	}
	if len(readiness.Shortfall) != 0 {
		t.Fatalf("shortfall = %+v, want none", readiness.Shortfall)
	}
}
