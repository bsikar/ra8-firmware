// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"strings"
	"testing"
)

// countedEvidence is an accumulation with one task in each section, so a test
// can break one counter on one task and nothing else on the page is wrong.
// Threshold 2: "alpha" disagreed once, "bravo" is graded once and short,
// "charlie" is graded twice and ready.
func countedEvidence(t *testing.T) (ShadowEvidence, ShadowReadiness) {
	t.Helper()
	first, err := CompareShadowRun([]ShadowObservation{
		observationOn(commitOne, "alpha", "failure", "success"),
		observationOn(commitOne, "bravo", "success", "success"),
		observationOn(commitOne, "charlie", "success", "success"),
	})
	if err != nil {
		t.Fatalf("compare first: %v", err)
	}
	second, err := CompareShadowRun([]ShadowObservation{
		observationOn(commitTwo, "bravo", "success", ""),
		observationOn(commitTwo, "charlie", "failure", "timed_out"),
	})
	if err != nil {
		t.Fatalf("compare second: %v", err)
	}
	evidence, err := AccumulateShadowEvidence([]ShadowReport{first, second})
	if err != nil {
		t.Fatalf("accumulate: %v", err)
	}
	readiness, err := evidence.Readiness(2)
	if err != nil {
		t.Fatalf("readiness: %v", err)
	}
	if len(readiness.Conflicting) != 1 || len(readiness.Insufficient) != 1 || len(readiness.Ready) != 1 {
		t.Fatalf("the fixture is not one task per section: %+v", readiness)
	}
	return evidence, readiness
}

const (
	commitOne = "1111111111111111111111111111111111111111"
	commitTwo = "2222222222222222222222222222222222222222"
)

func observationOn(commit, task, observed, actions string) ShadowObservation {
	return ShadowObservation{
		Task:              task,
		HeadSHA:           commit,
		ActionsJob:        task + " (ubuntu-latest)",
		Observed:          observed,
		ActionsConclusion: actions,
	}
}

// taskAt returns the index of a task in the accumulated evidence, so a test
// names the line it breaks rather than an ordinal.
func taskAt(t *testing.T, evidence ShadowEvidence, name string) int {
	t.Helper()
	for i, task := range evidence.Tasks {
		if task.Task == name {
			return i
		}
	}
	t.Fatalf("the fixture has no task %q", name)
	return -1
}

func refusedCountedEvidence(t *testing.T, evidence ShadowEvidence, readiness ShadowReadiness) error {
	t.Helper()
	var page strings.Builder
	err := RenderShadowEvidence(&page, evidence, readiness)
	if err == nil {
		t.Fatalf("the page rendered rather than being refused:\n%s", page.String())
	}
	if page.Len() != 0 {
		t.Fatalf("a refused render wrote to the stream: %q", page.String())
	}
	return err
}

// *** THE READING THIS CHECK EXISTS FOR. A ready task printed as "4 graded (1
// agreed, 1 divergent)" says it cleared the threshold and accounts for two of
// the commits that cleared it, in the section headed "ready (may move)", with
// no other source for the rest. ***
func TestAReadyTaskThatDoesNotAccountForItsGradingIsRefused(t *testing.T) {
	evidence, readiness := countedEvidence(t)
	evidence.Tasks[taskAt(t, evidence, "charlie")].Agreed = 0

	err := refusedCountedEvidence(t, evidence, readiness)
	if !strings.Contains(err.Error(), `"charlie" is graded on 2 and counts 0 agreed, 1 divergent, 0 disagreeing`) {
		t.Fatalf("err = %v, does not name the grading that does not add up", err)
	}
}

func TestATaskPairedOnMoreThanItAccountsForIsRefused(t *testing.T) {
	evidence, readiness := countedEvidence(t)
	evidence.Tasks[taskAt(t, evidence, "bravo")].Observed = 5

	err := refusedCountedEvidence(t, evidence, readiness)
	if !strings.Contains(err.Error(), `"bravo" is paired on 5 and counts 1 graded with 1 never judged`) {
		t.Fatalf("err = %v, does not name the pairing count that does not add up", err)
	}
}

// A task in the section for tasks to argue about, with nothing to argue about.
func TestAConflictingTaskCountingNoDisagreementIsRefused(t *testing.T) {
	evidence, readiness := countedEvidence(t)
	at := taskAt(t, evidence, "alpha")
	evidence.Tasks[at].Conflicting = 0
	evidence.Tasks[at].Agreed = 1

	err := refusedCountedEvidence(t, evidence, readiness)
	if !strings.Contains(err.Error(), `"alpha" is named as disagreeing and counts 0 disagreeing commits`) {
		t.Fatalf("err = %v, does not refuse a conflicting task with no conflict", err)
	}
}

// The numbers on a line are read before the commits that line names, the order
// #1663 settled for the insufficient line.
func TestTheCountsAreReadBeforeTheCommitsTheyName(t *testing.T) {
	evidence, readiness := countedEvidence(t)
	at := taskAt(t, evidence, "alpha")
	evidence.Tasks[at].Conflicting = 0
	evidence.Tasks[at].Agreed = 1
	evidence.Tasks[at].ConflictingCommits = []string{"3333333333333333333333333333333333333333"}

	err := refusedCountedEvidence(t, evidence, readiness)
	if !strings.Contains(err.Error(), "counts 0 disagreeing commits") {
		t.Fatalf("err = %v, does not refuse the count first", err)
	}
	if strings.Contains(err.Error(), "not one of the accumulated commits") {
		t.Fatalf("err = %v, refuses a commit list under a count that is already wrong", err)
	}
}

// *** A READY TASK'S COMMIT LISTS ARE STILL NOT READ: they are printed nowhere
// and counted nowhere, the scope checkNamedCommits settled. ***
func TestAReadyTaskKeepsItsUnprintedCommitsUnread(t *testing.T) {
	evidence, readiness := countedEvidence(t)
	evidence.Tasks[taskAt(t, evidence, "charlie")].IndeterminateCommits =
		[]string{"4444444444444444444444444444444444444444"}

	var page strings.Builder
	if err := RenderShadowEvidence(&page, evidence, readiness); err != nil {
		t.Fatalf("a ready task was refused over a list the page never prints: %v", err)
	}
}

// *** THE LENGTHS OF THE PRINTED COMMIT LISTS ARE DELIBERATELY NOT HELD TO
// THESE COUNTS. maxRenderedEvidenceCommits is enforced by writeCommitLine
// while the page is being written, after every check in this function, so a
// length check here would refuse an over-long list as a disagreement between a
// count and a list rather than as a page too long to read. The bounds tests
// pin that, and holding the lists to their counts means moving that bound
// forward first. ***
func TestACountWithMoreCommitsNamedThanItSaysStillRenders(t *testing.T) {
	evidence, readiness := countedEvidence(t)
	at := taskAt(t, evidence, "alpha")
	evidence.Tasks[at].ConflictingCommits = []string{commitOne, commitOne}

	var page strings.Builder
	if err := RenderShadowEvidence(&page, evidence, readiness); err != nil {
		t.Fatalf("the page was refused for a list length this check does not read: %v", err)
	}
}

func TestAnOrdinaryEvidencePageStillRenders(t *testing.T) {
	evidence, readiness := countedEvidence(t)

	var page strings.Builder
	if err := RenderShadowEvidence(&page, evidence, readiness); err != nil {
		t.Fatalf("an ordinary page was refused: %v", err)
	}
	for _, line := range []string{"alpha: 1 of 1", "bravo: graded on 1", "charlie: 2 graded"} {
		if !strings.Contains(page.String(), line) {
			t.Fatalf("the page does not carry %q:\n%s", line, page.String())
		}
	}
}
