// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"strings"
	"testing"
)

// contestedRunSurveyOf is one ordinary contested group: a stranger's runs
// under names tasks plan. The listing is filled in from the group because
// the page reads one against the other.
func contestedRunSurveyOf(runs int) reconcileReport {
	return standingSurveyOf([]reconcileContestedStanding{
		contestedStandingOf("foreign", runs),
	}, nil)
}

// The name is the point of the section. A stranger's run under a name we
// plan is a name branch protection may one day require, held by somebody
// else, and "#7 under  (build)" is that finding with the answer missing.
func TestAContestedRunThatNamesNoCheckRunIsRefused(t *testing.T) {
	report := contestedRunSurveyOf(2)
	report.ContestedStanding[0].Runs[1].Name = ""

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "names no check run") {
		t.Fatalf("the refusal does not say the check run is missing: %v", err)
	}
	if !strings.Contains(err.Error(), "foreign") {
		t.Fatalf("the refusal does not say where the run stands: %v", err)
	}
}

// Whitespace is not a statement, the rule the commit and the mode keep: a
// name of three spaces prints as a blank one.
func TestAContestedRunWhoseCheckRunIsBlankSpaceIsRefused(t *testing.T) {
	report := contestedRunSurveyOf(1)
	report.ContestedStanding[0].Runs[0].Name = "   "

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "names no check run") {
		t.Fatalf("the refusal does not say the check run is missing: %v", err)
	}
}

// The task is the other half of the line: it says which of our plans wanted
// that name, and it renders in the parentheses a reader looks in for it.
func TestAContestedRunThatNamesNoTaskIsRefused(t *testing.T) {
	report := contestedRunSurveyOf(1)
	report.ContestedStanding[0].Runs[0].Task = ""

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "names no task") {
		t.Fatalf("the refusal does not say the task is missing: %v", err)
	}
}

// A run that states neither is refused as the missing check run: that is
// what the reader goes looking for, and the task only says which plan
// wanted it.
func TestTheCheckRunIsReadBeforeTheTask(t *testing.T) {
	report := contestedRunSurveyOf(1)
	report.ContestedStanding[0].Runs[0].Name = ""
	report.ContestedStanding[0].Runs[0].Task = ""

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "names no check run") {
		t.Fatalf("the refusal does not lead with the check run: %v", err)
	}
	if strings.Contains(err.Error(), "names no task") {
		t.Fatalf("the refusal answers for the task as well: %v", err)
	}
}

// The standing is read before the runs it groups: a group that carries no
// standing is refused as that, not as a run under nothing.
func TestAGroupThatCarriesNoStandingIsReadBeforeItsRuns(t *testing.T) {
	report := contestedRunSurveyOf(1)
	report.ContestedStanding[0].Identifier = ""
	report.ContestedStanding[0].Runs[0].Name = ""

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "carries no standing") {
		t.Fatalf("the refusal does not lead with the standing: %v", err)
	}
}

// An unplanned standing prints its runs as bare numbers, so the name in
// that listing is a field nobody would have read here. This is the same
// rule that leaves a settled task's fields alone.
func TestAnUnplannedRunWithNoNameIsStated(t *testing.T) {
	report := standingSurveyOf(nil, []reconcileUnplannedStanding{
		unplannedStandingOf("a stranger", 2),
	})
	for run := range report.UnplannedRun {
		report.UnplannedRun[run].Name = ""
	}

	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("render: %v", err)
	}
	if !strings.Contains(page.String(), "no task plans, a stranger:") {
		t.Fatalf("page %q does not state the standing", page)
	}
}

// Nothing an ordinary contested group carries is refused, and the line still
// names the check run and the task it sits under.
func TestAnOrdinaryContestedGroupIsStated(t *testing.T) {
	report := contestedRunSurveyOf(2)

	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("render: %v", err)
	}
	if !strings.Contains(page.String(), "under a name we plan, foreign:") {
		t.Fatalf("page %q does not state the standing", page)
	}
	if !strings.Contains(page.String(), "under ra8ci / build (build)") {
		t.Fatalf("page %q does not name the check run and the task", page)
	}
}
