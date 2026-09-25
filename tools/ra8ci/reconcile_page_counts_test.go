// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// countedSurveyOf builds a survey with the given task decisions and the
// counts that match them, so a test can move one count on its own. The
// decisions are what the page counts, so they are set here rather than
// surveyed: a real listing cannot produce a count that disagrees with its
// own tasks, which is exactly the document this check exists to refuse.
func countedSurveyOf(decisions ...github.PublishDecision) reconcileReport {
	report := reconcileReport{
		Commit: strings.Repeat("a", 40),
		Mode:   "authoritative",
	}
	for index, decision := range decisions {
		report.Tasks = append(report.Tasks, reconcileSurvey{
			Task:     "build",
			Name:     "ra8ci / build",
			Decision: decisionToken(decision),
		})
		_ = index
		switch decision {
		case github.PublishNeeded:
			report.Posting++
		case github.PublishInFlight:
			report.Waiting++
		case github.PublishConflicts:
			report.Conflict++
		}
	}
	report.Settled = report.Posting == 0 && report.Waiting == 0 && report.Conflict == 0
	return report
}

// refusedCountedPage renders a survey that must be refused as unstateable and
// answers with the refusal. Nothing may be written above it.
func refusedCountedPage(t *testing.T, report reconcileReport) error {
	t.Helper()
	page := &bytes.Buffer{}
	err := RenderReconcileSurvey(page, report)
	if !errors.Is(err, ErrReconcilePageInvalid) {
		t.Fatalf("err = %v, want a refusal", err)
	}
	if page.Len() != 0 {
		t.Fatalf("page %q was written above the refusal", page)
	}
	return err
}

// The count of tasks to post is checked like the conflicting one. It is
// printed on the second line and no line below it names those tasks, so a
// reader cannot catch it themselves.
func TestASurveyThatMiscountsWhatIsToPostIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishNeeded)
	report.Posting = 3
	report.Settled = false

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "3 tasks to post counted, 1 named") {
		t.Fatalf("err = %v, does not say what was counted and what was found", err)
	}
}

// The same for the tasks waiting on a write already in flight.
func TestASurveyThatMiscountsWhatIsInFlightIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishInFlight)
	report.Waiting = 0
	report.Settled = true

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "0 tasks in flight counted, 1 named") {
		t.Fatalf("err = %v, does not say what was counted and what was found", err)
	}
}

// The conflicting count keeps the refusal it already had, word for word: the
// wording is what an operator reading this page has learned to look for.
func TestTheConflictingCountKeepsItsRefusal(t *testing.T) {
	report := countedSurveyOf(github.PublishConflicts)
	report.Conflict = 2

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "2 conflicting tasks counted, 1 named") {
		t.Fatalf("err = %v, is not the refusal this page already gave", err)
	}
}

// The verdict is the line read first, so a settled verdict over work still
// to do is refused rather than printed above the lines that contradict it.
func TestASettledVerdictOverWorkToDoIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishConflicts)
	report.Settled = true

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "settled is true over 0 to post, 0 in flight, 1 conflicting") {
		t.Fatalf("err = %v, does not say the verdict disagrees with the counts", err)
	}
}

// And the other way round: a commit with nothing to do that says it is not
// settled sends an operator looking for work that is not there.
func TestAnUnsettledVerdictOverNothingToDoIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishSettled)
	report.Settled = false

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "settled is false over 0 to post, 0 in flight, 0 conflicting") {
		t.Fatalf("err = %v, does not say the verdict disagrees with the counts", err)
	}
}

// The runs no task plans are counted in the document and named on the page
// through their standings, so a count that disagrees with the listing is the
// same contradiction one field further out.
func TestASurveyThatMiscountsTheUnplannedRunsIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishSettled)
	report.Unplanned = 2

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "2 runs no task plans counted, 0 listed") {
		t.Fatalf("err = %v, does not say what was counted and what was listed", err)
	}
}

// A settled publish carrying a leftover run is ORDINARY and must not be
// refused: the unplanned count deliberately moves neither the verdict nor
// the exit status, and the page states both facts.
func TestASettledSurveyWithALeftoverRunIsStated(t *testing.T) {
	report := countedSurveyOf(github.PublishSettled)
	report.UnplannedRun = []reconcileUnplannedRun{{ID: 7, Name: "other / build", Identifier: "foreign"}}
	report.Unplanned = 1
	report.UnplannedStanding = []reconcileUnplannedStanding{{Identifier: "foreign", Runs: []int64{7}}}

	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("an ordinary survey was refused: %v", err)
	}
	if !strings.Contains(page.String(), "settled: every planned task is accounted for") {
		t.Fatalf("page %q does not state the verdict", page)
	}
	if !strings.Contains(page.String(), "no task plans, foreign: #7") {
		t.Fatalf("page %q does not name the leftover run", page)
	}
}

// A survey the surveying command produced is never refused by these checks.
// They are about a document contradicting itself, not about the survey being
// wrong, and a check that refuses this plane's own output is a bug.
func TestASurveysOwnCountsAreNotRefused(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	report := surveyOf(t, []plannedCheckRun{plan}, listing())

	if err := checkReconcileSurveyCounts(report); err != nil {
		t.Fatalf("the survey refused its own counts: %v", err)
	}
}

// The bounds are read before the counts: a survey too large to state is
// refused for its size, whatever its counts say.
func TestTheBoundsAreReadBeforeTheCountsCheck(t *testing.T) {
	report := countedSurveyOf(github.PublishSettled)
	report.Conflict = 9
	report.Tasks = make([]reconcileSurvey, maxRenderedSurveyTasks+1)

	page := &bytes.Buffer{}
	err := RenderReconcileSurvey(page, report)
	if !errors.Is(err, ErrReconcilePageTooLarge) {
		t.Fatalf("err = %v, want the size refusal", err)
	}
}
