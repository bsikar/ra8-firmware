// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

// subjectSurveyHead is the commit these candidates sit on where the test is
// not about the commit itself.
const subjectSurveyHead = "5555555555555555555555555555555555555555"

// The workflow is on the line read first, in both readings of it. A survey
// without one states a verdict about nothing over a set about to be gathered.
func TestASurveyThatNamesNoWorkflowIsRefused(t *testing.T) {
	report := caveatSurveyOf(caveatSelection(1589, subjectSurveyHead))
	report.Workflow = ""

	err := refusedSurveyPage(t, report)
	if !strings.Contains(err.Error(), "names no workflow") {
		t.Fatalf("the refusal does not say the workflow is missing: %v", err)
	}
}

// Whitespace is not a statement, the rule this page already keeps for a
// commit: a workflow of three spaces would print as a blank one.
func TestASurveyWhoseWorkflowIsBlankSpaceIsRefused(t *testing.T) {
	report := caveatSurveyOf(caveatSelection(1589, subjectSurveyHead))
	report.Workflow = "   "

	err := refusedSurveyPage(t, report)
	if !strings.Contains(err.Error(), "names no workflow") {
		t.Fatalf("the refusal does not say the workflow is missing: %v", err)
	}
}

// A selection's commit is the one the gather opens its run on. "selected:
// #1589 at , run 771 attempt 1" names a run and nowhere to find it.
func TestASelectionAtNoCommitIsRefused(t *testing.T) {
	report := caveatSurveyOf(caveatSelection(1589, ""))

	err := refusedSurveyPage(t, report)
	if !strings.Contains(err.Error(), "#1589 is at no commit") {
		t.Fatalf("the refusal does not name the candidate at no commit: %v", err)
	}
}

// The dropped half of the listing is printed with its commit too, and an
// operator is being asked to leave that candidate out of the gather.
func TestADroppedCandidateAtNoCommitIsRefused(t *testing.T) {
	report := caveatSurveyOf(droppedSelection(1591, "   "))

	err := refusedSurveyPage(t, report)
	if !strings.Contains(err.Error(), "#1591 is at no commit") {
		t.Fatalf("the refusal does not name the candidate at no commit: %v", err)
	}
}

// Read before the shared heads. That check compares a candidate's head with
// the commit it is grouped under, so a candidate at no commit would be
// refused as "#1589 shares 5555... and is at", a sentence with a gap where
// the answer goes.
func TestACandidateAtNoCommitIsReadBeforeItsSharedHead(t *testing.T) {
	blank := caveatSelection(1589, "")
	other := caveatSelection(1590, subjectSurveyHead)
	report := sharedHeadSurveyOf([]surveySharedHead{{
		HeadSHA:      subjectSurveyHead,
		PullRequests: []int{1589, 1590},
	}}, blank, other)

	err := refusedSurveyPage(t, report)
	if !strings.Contains(err.Error(), "#1589 is at no commit") {
		t.Fatalf("the subject was not read before the shared heads: %v", err)
	}
	if strings.Contains(err.Error(), "shares") {
		t.Fatalf("the refusal is the shared head one: %v", err)
	}
}

// Read after the listing. A candidate numbered zero is still refused as
// unnumbered, naming the unstated commit: the number is what a reader opens,
// and the commit is all that is left to identify it with.
func TestAnUnnumberedCandidateKeepsItsUnstatedCommit(t *testing.T) {
	report := caveatSurveyOf(caveatSelection(0, ""))

	err := refusedSurveyPage(t, report)
	if !strings.Contains(err.Error(), "an unstated commit") {
		t.Fatalf("the refusal is not the unnumbered one: %v", err)
	}
}

// Nothing a real survey writes is refused: the workflow is the argument the
// command was given and the heads come off the pull requests themselves.
func TestACandidateSurveysOwnSubjectIsNotRefused(t *testing.T) {
	page := renderCaveatSurvey(t, caveatSurveyOf(
		caveatSelection(1589, subjectSurveyHead),
		droppedSelection(1591, "6666666666666666666666666666666666666666"),
	))
	if !strings.Contains(page, "ra8ci") {
		t.Fatalf("page %q does not name the workflow", page)
	}
	if !strings.Contains(page, subjectSurveyHead) {
		t.Fatalf("page %q does not name the commit", page)
	}
}
