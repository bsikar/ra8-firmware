// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// decidedSurveyOf builds a settled one-task survey whose decision a test can
// then replace. countedSurveyOf writes the decision with decisionToken, which
// is exactly what a document carrying a decision this build cannot state
// never did, so the token is set directly here.
func decidedSurveyOf(decision string) reconcileReport {
	report := countedSurveyOf(github.PublishSettled)
	report.Tasks[0].Decision = decision
	return report
}

// A decision outside the four the survey writes is refused. It is counted
// under none of the three buckets, so the counts still agree and the verdict
// still comes out settled: the page would open "settled: every planned task
// is accounted for" over a task it accounts for nowhere.
func TestATaskWhoseDecisionIsNotWrittenHereIsRefused(t *testing.T) {
	report := decidedSurveyOf("published")

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "task build carries a decision this survey does not write: published") {
		t.Fatalf("err = %v, does not name the task and the decision", err)
	}
}

// A task stating no decision at all is refused saying so, rather than as a
// decision nobody writes: it is the one an operator can act on.
func TestATaskThatStatesNoDecisionIsRefused(t *testing.T) {
	report := decidedSurveyOf("")

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "task build states no decision") {
		t.Fatalf("err = %v, does not say the decision is missing", err)
	}
	if strings.Contains(err.Error(), "does not write") {
		t.Fatalf("err = %v, refused as an unwritten decision rather than a missing one", err)
	}
}

// Whitespace around a decision is refused, and deliberately NOT as a blank
// one. The counts are keyed by the raw string, so " conflicts" is counted
// under nothing; trimming it here would state it as well formed and leave
// the count it defeats to a check that cannot see it either.
func TestADecisionPaddedWithSpaceIsRefused(t *testing.T) {
	report := decidedSurveyOf(" " + decisionToken(github.PublishConflicts))

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "does not write: conflicts") {
		t.Fatalf("err = %v, does not name the padded decision", err)
	}
}

// A task the page never prints is still read. A settled task's name appears
// nowhere on the page, and its decision is still what the total and the
// verdict are claims about.
func TestASettledTaskWithNoDecisionIsRefusedThoughItIsNeverPrinted(t *testing.T) {
	report := countedSurveyOf(github.PublishSettled, github.PublishSettled)
	report.Tasks[1].Decision = "accounted"

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "does not write: accounted") {
		t.Fatalf("err = %v, did not read the task the page never prints", err)
	}
}

// An unnamed task is said to be unnamed rather than left as a gap in the
// sentence. The subject check reads only the conflicting tasks, so a task
// this one refuses may carry no name at all.
func TestAnUnnamedTaskIsRefusedByName(t *testing.T) {
	report := decidedSurveyOf("published")
	report.Tasks[0].Task = "  "

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "an unnamed task carries a decision") {
		t.Fatalf("err = %v, does not say the task is unnamed", err)
	}
}

// The decisions are read BEFORE the counts, because the counts are read
// through the decision and a decision nobody writes defeats them silently.
func TestTheDecisionsAreReadBeforeTheCounts(t *testing.T) {
	report := decidedSurveyOf("published")
	report.Posting = 7

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "does not write: published") {
		t.Fatalf("err = %v, want the decision refusal", err)
	}
	if strings.Contains(err.Error(), "counted") {
		t.Fatalf("err = %v, refused as a miscount rather than as the decision", err)
	}
}

// The listings are still read before the decisions. A run listed twice is
// about which runs exist, and it is refused as that whatever the decisions
// say.
func TestARepeatedRunIsReadBeforeTheDecisions(t *testing.T) {
	report := decidedSurveyOf("published")
	report.UnplannedRun = []reconcileUnplannedRun{{ID: 7}, {ID: 7}}
	report.Unplanned = 2

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 7 is listed more than once") {
		t.Fatalf("err = %v, want the repeated listing", err)
	}
}

// Every decision the survey does write is stated, and a survey of them all
// renders.
func TestASurveyOfEveryDecisionTheSurveyWritesIsStated(t *testing.T) {
	report := countedSurveyOf(
		github.PublishNeeded,
		github.PublishSettled,
		github.PublishInFlight,
		github.PublishConflicts,
	)

	page := renderedSurvey(t, report)
	if !strings.Contains(page, "4 tasks, 1 to post, 1 in flight, 1 conflicting") {
		t.Fatalf("page = %q, does not state the four decisions", page)
	}
}
