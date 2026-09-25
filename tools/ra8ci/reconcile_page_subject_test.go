// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// conflictingSurveyOf is a survey with one conflicting task, the shape whose
// line names a task and a check run.
func conflictingSurveyOf() reconcileReport {
	return countedSurveyOf(github.PublishConflicts)
}

// The commit is on the line read first and was carried straight out of the
// document. A blank one renders a clean verdict about nothing.
func TestASurveyThatNamesNoCommitIsRefused(t *testing.T) {
	report := countedSurveyOf()
	report.Commit = ""

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "names no commit") {
		t.Fatalf("the refusal does not say the commit is missing: %v", err)
	}
}

// Whitespace is not a statement: a commit of three spaces would print as a
// blank one and is refused as one.
func TestASurveyWhoseCommitIsBlankSpaceIsRefused(t *testing.T) {
	report := countedSurveyOf()
	report.Commit = "   "

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "names no commit") {
		t.Fatalf("the refusal does not say the commit is missing: %v", err)
	}
}

// The mode is the other half of the line read first, and it is what says
// whether a publish can hold a pull request at all.
func TestASurveyThatNamesNoModeIsRefused(t *testing.T) {
	report := countedSurveyOf()
	report.Mode = ""

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "names no mode") {
		t.Fatalf("the refusal does not say the mode is missing: %v", err)
	}
	if !strings.Contains(err.Error(), report.Commit) {
		t.Fatalf("the refusal does not say which survey: %v", err)
	}
}

// A conflicting line is the one line an operator is meant to act on, and it
// names a task and the check run under it.
func TestAConflictingTaskThatIsNotNamedIsRefused(t *testing.T) {
	report := conflictingSurveyOf()
	report.Tasks[0].Task = ""

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "a conflicting task is not named") {
		t.Fatalf("the refusal does not say the task is missing: %v", err)
	}
}

// The check run's name is how an operator finds the run on the commit, so a
// conflicting task that states none is refused rather than printed bare.
func TestAConflictingTaskWithNoCheckRunNameIsRefused(t *testing.T) {
	report := conflictingSurveyOf()
	report.Tasks[0].Name = "  "

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "conflicting task build names no check run") {
		t.Fatalf("the refusal does not name the task missing its run: %v", err)
	}
}

// Only the tasks the page NAMES are read. A settled task is never printed, so
// refusing the whole page over a field nobody would have seen takes a
// readable page away from an operator for nothing.
func TestASettledTaskThatStatesNothingIsStillRendered(t *testing.T) {
	report := countedSurveyOf(github.PublishSettled)
	report.Tasks[0].Task = ""
	report.Tasks[0].Name = ""

	page := renderedSurvey(t, report)
	if !strings.HasPrefix(page, "settled:") {
		t.Fatalf("page %q does not state the verdict", page)
	}
	if strings.Contains(page, "conflicting:") {
		t.Fatalf("page %q names a settled task", page)
	}
}

// An identifier is the whole of what groups a standing's runs. A blank one
// prints "no task plans, : #7" and sends the reader nowhere.
func TestAnUnplannedGroupThatCarriesNoStandingIsRefused(t *testing.T) {
	report := standingSurveyOf(nil, []reconcileUnplannedStanding{unplannedStandingOf("", 2)})

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "no task plans, a group of 2 runs carries no standing") {
		t.Fatalf("the refusal does not say which group: %v", err)
	}
}

// The planned half of the same finding.
func TestAContestedGroupThatCarriesNoStandingIsRefused(t *testing.T) {
	report := standingSurveyOf([]reconcileContestedStanding{contestedStandingOf("", 1)}, nil)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "under a name we plan, a group of 1 runs carries no standing") {
		t.Fatalf("the refusal does not say which group: %v", err)
	}
}

// The bounds are read first: a page too large to state is refused as that
// whatever else is wrong with it, because the reader's move is the same.
func TestTheBoundsAreReadBeforeTheSubject(t *testing.T) {
	report := standingSurveyOf(nil, []reconcileUnplannedStanding{
		unplannedStandingOf("foreign", maxRenderedSurveyRuns+1),
	})
	report.Commit = ""

	err := refusedReconcilePage(t, report)
	if !strings.Contains(err.Error(), "at most") {
		t.Fatalf("the bound was not read first: %v", err)
	}
}

// The subject is read before the listings. A survey that does not say which
// commit it is about is refused as that rather than as a repeated run on a
// commit it never named.
func TestTheSubjectIsReadBeforeTheRuns(t *testing.T) {
	report := countedSurveyOf()
	report.Commit = ""
	report.UnplannedRun = []reconcileUnplannedRun{
		unplannedRunOf(7, "foreign"),
		unplannedRunOf(7, "superseded"),
	}
	report.Unplanned = len(report.UnplannedRun)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "names no commit") {
		t.Fatalf("the subject was not read first: %v", err)
	}
}

// Nothing a real survey writes is refused: the commit and the mode come from
// the arguments the command was given and the names come from the catalog.
func TestASurveysOwnSubjectIsNotRefused(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	foreign := publishedAs(plan, 313, "completed", "success", plan.Run.Title)
	foreign.ExternalID = "someone-else"

	page := renderedSurvey(t, surveyOf(t, []plannedCheckRun{plan}, listing(foreign)))
	if !strings.Contains(page, plan.Run.HeadSHA) {
		t.Fatalf("page %q does not name the commit", page)
	}
}
