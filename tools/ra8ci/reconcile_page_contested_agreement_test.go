// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"strings"
	"testing"
)

// agreeingSurveyOf is one ordinary contested group whose listing and
// standing say the same two words about every run, which is the shape
// contestedStandings produces.
func agreeingSurveyOf(runs ...int64) reconcileReport {
	return standingSurveyOf([]reconcileContestedStanding{
		contestedGroupOf("foreign", runs...),
	}, nil)
}

// The check run is what the line sends the reader to open. A standing that
// names one the listing never published the run under is a name no task in
// the document plans, printed with the page's own authority behind it.
func TestAContestedRunUnderAnotherCheckRunThanTheListingIsRefused(t *testing.T) {
	report := agreeingSurveyOf(7)
	report.ContestedStanding[0].Runs[0].Name = "ra8ci / lint"

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "ra8ci / lint") {
		t.Fatalf("the refusal does not say what the standing states: %v", err)
	}
	if !strings.Contains(err.Error(), "ra8ci / build") {
		t.Fatalf("the refusal does not say what the listing publishes: %v", err)
	}
	if !strings.Contains(err.Error(), "run 7") {
		t.Fatalf("the refusal does not name the run: %v", err)
	}
}

// The task is the other half of the line, the parentheses a reader looks in
// to see which of our plans wanted the name.
func TestAContestedRunUnderAnotherTaskThanTheListingIsRefused(t *testing.T) {
	report := agreeingSurveyOf(7)
	report.ContestedStanding[0].Runs[0].Task = "lint"

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "task lint") {
		t.Fatalf("the refusal does not say what the standing states: %v", err)
	}
	if !strings.Contains(err.Error(), "task build") {
		t.Fatalf("the refusal does not say what the listing publishes: %v", err)
	}
}

// A run that disagrees about both is refused as the check run, the order
// the two blank cases already keep: the check run is what is opened and the
// task only says which plan wanted it.
func TestTheCheckRunIsReadBeforeTheTaskWhenNeitherAgrees(t *testing.T) {
	report := agreeingSurveyOf(7)
	report.ContestedStanding[0].Runs[0].Name = "ra8ci / lint"
	report.ContestedStanding[0].Runs[0].Task = "lint"

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "ra8ci / lint") {
		t.Fatalf("the refusal does not lead with the check run: %v", err)
	}
	if strings.Contains(err.Error(), "task lint") {
		t.Fatalf("the refusal answers for the task as well: %v", err)
	}
}

// Casing is not folded. A check run name is GitHub's own string and it is
// what a reader types into the Checks tab, so two casings are two answers.
func TestAContestedCheckRunThatDiffersOnlyInCasingIsRefused(t *testing.T) {
	report := agreeingSurveyOf(7)
	report.ContestedStanding[0].Runs[0].Name = "RA8CI / Build"

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "RA8CI / Build") {
		t.Fatalf("the refusal does not say what the standing states: %v", err)
	}
}

// Where the run stands is read before what it is called. A run grouped
// under an identifier it does not carry is refused as that, whatever the
// two words beside it say.
func TestTheStandingIsReadBeforeTheNamesItStates(t *testing.T) {
	report := agreeingSurveyOf(7)
	report.Tasks[0].Published[0].Identifier = "superseded"
	report.ContestedStanding[0].Runs[0].Name = "ra8ci / lint"

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "stands superseded") {
		t.Fatalf("the refusal does not lead with where the run stands: %v", err)
	}
	if strings.Contains(err.Error(), "ra8ci / lint") {
		t.Fatalf("the refusal answers for the name as well: %v", err)
	}
}

// A run the listing does not carry at all is refused as unsurveyed by the
// grouping check, which is why the agreement walk never reads a run it has
// no listing for.
func TestAnUnsurveyedContestedRunIsReadBeforeItsName(t *testing.T) {
	report := agreeingSurveyOf(7)
	report.Tasks = nil
	report.ContestedStanding[0].Runs[0].Name = "ra8ci / lint"

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "was not surveyed") {
		t.Fatalf("the refusal does not say the run was not surveyed: %v", err)
	}
}

// An unplanned group's runs are bare numbers on this page and carry no
// names to agree about, the rule the blank checks already keep.
func TestAnUnplannedGroupCarriesNoNamesToAgreeWith(t *testing.T) {
	report := standingSurveyOf(nil, []reconcileUnplannedStanding{
		unplannedGroupOf("foreign", 9),
	})

	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("an ordinary unplanned group was refused: %v", err)
	}
	if !strings.Contains(page.String(), "no task plans, foreign: #9") {
		t.Fatalf("the group is not stated: %q", page)
	}
}

// The ordinary shape, the one contestedStandings builds, still renders.
func TestAContestedRunThatAgreesWithItsListingIsStated(t *testing.T) {
	report := agreeingSurveyOf(7, 9)

	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("an agreeing group was refused: %v", err)
	}
	if !strings.Contains(page.String(), "under a name we plan, foreign: #7 under ra8ci / build (build), #9 under ra8ci / build (build)") {
		t.Fatalf("the group is not stated: %q", page)
	}
}
