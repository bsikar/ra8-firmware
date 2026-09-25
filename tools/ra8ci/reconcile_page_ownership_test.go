// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// ownedRunOf is one run a task published, carrying both answers about who
// posted it: the bit the document is read by and the standing the page's
// groupings are built from.
func ownedRunOf(id int64, ours bool, identifier string) reconcileSurveyedRun {
	return reconcileSurveyedRun{
		ID: id, Status: "completed", Conclusion: "success",
		Ours: ours, Identifier: identifier,
	}
}

// ownedLeftoverOf is the same pair on a run no task plans.
func ownedLeftoverOf(id int64, ours bool, identifier string) reconcileUnplannedRun {
	run := unplannedRunOf(id, identifier)
	run.Ours = ours
	return run
}

// A run reported as ours under a foreign standing is the worst of the two
// directions: the page names it as somebody else publishing under a name we
// plan, and whatever reads the document is handed it as our own run.
func TestARunReportedAsOursOverAForeignStandingIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishConflicts)
	report.Tasks[0].Published = []reconcileSurveyedRun{ownedRunOf(11, true, "foreign")}

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 11 is published under a name we plan, is reported as ours, and stands foreign") {
		t.Fatalf("the refusal does not say what the two answers were: %v", err)
	}
}

// THE OTHER DIRECTION IS PINNED OPEN DELIBERATELY. A run standing ours whose
// bit is false is indistinguishable from a document that never carried the
// field: a plain bool decodes the same from an absent key and a stated
// false, and this page reads documents other builds wrote.
func TestARunStandingOursWithNoClaimStillRenders(t *testing.T) {
	report := countedSurveyOf(github.PublishSettled)
	report.Tasks[0].Published = []reconcileSurveyedRun{
		ownedRunOf(12, false, github.ExternalIDOurs.String()),
	}

	page := renderedSurvey(t, report)
	if strings.Contains(page, "under a name we plan") {
		t.Fatalf("our own run was grouped as contested:\n%s", page)
	}
}

// The leftover listing carries the same pair and is read the same way. A
// leftover reported as ours under a superseded standing is a run nobody can
// place, printed on the page as one this deployment cannot account for.
func TestALeftoverReportedAsOursOverASupersededStandingIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishSettled)
	report.UnplannedRun = []reconcileUnplannedRun{ownedLeftoverOf(7, true, "superseded")}
	report.Unplanned = len(report.UnplannedRun)
	report.UnplannedStanding = []reconcileUnplannedStanding{{
		Identifier: "superseded", Runs: []int64{7},
	}}

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 7 is listed as one no task plans, is reported as ours, and stands superseded") {
		t.Fatalf("the refusal does not say what the two answers were: %v", err)
	}
}

// A leftover of ours under a retired name is the ordinary case this page
// exists to state, and both answers agree about it.
func TestALeftoverOfOursUnderARetiredNameIsStated(t *testing.T) {
	ours := github.ExternalIDOurs.String()
	report := countedSurveyOf(github.PublishSettled)
	report.UnplannedRun = []reconcileUnplannedRun{ownedLeftoverOf(8, true, ours)}
	report.Unplanned = len(report.UnplannedRun)
	report.UnplannedStanding = []reconcileUnplannedStanding{{
		Identifier: ours, Runs: []int64{8},
	}}

	page := renderedSurvey(t, report)
	if !strings.Contains(page, "no task plans, ours: #8") {
		t.Fatalf("an ordinary leftover of ours was not stated:\n%s", page)
	}
}

// A blank standing on a run claimed as ours is still a contradiction, and
// the refusal says the standing is unstated rather than leaving a gap where
// the answer should be.
func TestARunReportedAsOursUnderNoStatedStandingIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishConflicts)
	report.Tasks[0].Published = []reconcileSurveyedRun{ownedRunOf(13, true, "")}

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 13 is published under a name we plan, is reported as ours, and stands under no stated standing") {
		t.Fatalf("the refusal does not state the missing standing: %v", err)
	}
}

// A blank standing on the other side needs no rule here: a run not standing
// ours is grouped as contested, and a group carrying no standing is already
// refused by the grouping checks above.
func TestAContestedRunUnderNoStatedStandingIsRefusedAsAGroup(t *testing.T) {
	report := countedSurveyOf(github.PublishConflicts)
	report.Tasks[0].Published = []reconcileSurveyedRun{ownedRunOf(14, false, "")}
	report.ContestedStanding = []reconcileContestedStanding{{
		Identifier: "", Runs: []reconcileContestedRun{{
			ID: 14, Task: "build", Name: "ra8ci / build",
		}},
	}}

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "carries no standing") {
		t.Fatalf("the refusal is not the grouping one: %v", err)
	}
}

// The runs a task published are read before the leftovers, the order the
// page states the two groupings in, so a survey wrong in both places is
// refused for the one a reader reaches first.
func TestTheTaskListingIsReadBeforeTheLeftovers(t *testing.T) {
	report := countedSurveyOf(github.PublishConflicts)
	report.Tasks[0].Published = []reconcileSurveyedRun{ownedRunOf(15, true, "foreign")}
	report.UnplannedRun = []reconcileUnplannedRun{ownedLeftoverOf(9, true, "superseded")}
	report.Unplanned = len(report.UnplannedRun)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 15") {
		t.Fatalf("the refusal is not about the run a reader meets first: %v", err)
	}
	if strings.Contains(err.Error(), "run 9") {
		t.Fatalf("the refusal answers for the leftover listing: %v", err)
	}
}

// Nothing a real survey writes is refused. The two derivations ask the same
// question about the same name and commit, over a listing carrying a run of
// ours, a stranger's run under a name we plan and a leftover.
func TestASurveyBuiltFromRealRunsAgreesWithItself(t *testing.T) {
	first, second := twoCatalogTasks(t)
	mine := plannedRun(t, github.ModeAuthoritative, first, "failed")
	settled := publishedAs(mine, 201, "completed", mine.Run.Conclusion, mine.Run.Title)
	other := plannedRun(t, github.ModeAuthoritative, second, "failed")
	stranger := publishedAs(other, 202, "completed", other.Run.Conclusion, other.Run.Title)
	stranger.ExternalID = ""
	retired := publishedAs(other, 203, "completed", other.Run.Conclusion, other.Run.Title)
	retired.Name = other.Run.Name + " (retired)"

	report := surveyOf(t, []plannedCheckRun{mine, other}, listing(settled, stranger, retired))
	if err := checkSurveyedRunOwnership(report); err != nil {
		t.Fatalf("a survey of real runs was refused: %v", err)
	}
	ours := github.ExternalIDOurs.String()
	seen := 0
	for _, task := range report.Tasks {
		for _, run := range task.Published {
			seen++
			if run.Ours != (run.Identifier == ours) {
				t.Fatalf("run %d reports ours %v and stands %s", run.ID, run.Ours, run.Identifier)
			}
		}
	}
	if seen != 2 {
		t.Fatalf("the listing carried %d surveyed runs, want 2", seen)
	}
	if len(report.UnplannedRun) != 1 {
		t.Fatalf("the listing carried %d leftovers, want 1", len(report.UnplannedRun))
	}
}
