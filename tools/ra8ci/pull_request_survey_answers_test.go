// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"errors"
	"strings"
	"testing"
)

// answeredSurveyHead is the commit the candidates in this file sit on unless
// a test is about a second one.
const answeredSurveyHead = "cccccccccccccccccccccccccccccccccccccccc"

// droppedSelection is an ordinary unselectable candidate: surveyed, with a
// reason the page can state. Each test changes only the answer it is about.
func droppedSelection(number int, head string) surveyedPullRequest {
	dropped := caveatSelection(number, head)
	dropped.Selectable = false
	dropped.RunID = 0
	dropped.Attempt = 0
	dropped.Event = ""
	dropped.Conclusion = ""
	dropped.Reason = "no run for Checks on " + head
	return dropped
}

// A dropped candidate's reason is the only thing on its line that says why it
// is being dropped, and it appears nowhere else in the document.
func TestAnUnselectableCandidateWithNoReasonIsRefused(t *testing.T) {
	dropped := droppedSelection(1591, answeredSurveyHead)
	dropped.Reason = ""

	err := refusedSurveyPage(t, caveatSurveyOf(
		caveatSelection(1589, "1111111111111111111111111111111111111111"),
		dropped,
	))
	if !errors.Is(err, ErrPullRequestSurveyPageInvalid) {
		t.Fatalf("a candidate dropped for nothing was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "#1591 is unselectable and no reason is given") {
		t.Fatalf("the refusal does not name the candidate: %v", err)
	}
}

// A reason of nothing but space is no reason: the line would print an empty
// pair of brackets either way.
func TestAnUnselectableCandidateWhoseReasonIsSpaceIsRefused(t *testing.T) {
	dropped := droppedSelection(1591, answeredSurveyHead)
	dropped.Reason = "   "

	err := refusedSurveyPage(t, caveatSurveyOf(dropped))
	if !errors.Is(err, ErrPullRequestSurveyPageInvalid) {
		t.Fatalf("a blank reason was not refused: %v", err)
	}
}

// The selections are the set to hand on, and each line names the run the
// gather would open.
func TestASelectionWithNoRunIsRefused(t *testing.T) {
	selection := caveatSelection(1589, answeredSurveyHead)
	selection.RunID = 0

	err := refusedSurveyPage(t, caveatSurveyOf(selection))
	if !errors.Is(err, ErrPullRequestSurveyPageInvalid) {
		t.Fatalf("a selection naming no run was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "#1589 is selectable and names no run") {
		t.Fatalf("the refusal does not name the candidate: %v", err)
	}
}

// A run identifier comes off the run GitHub answered with, so a negative one
// is the same finding as none at all.
func TestASelectionAtANegativeRunIsRefused(t *testing.T) {
	selection := caveatSelection(1589, answeredSurveyHead)
	selection.RunID = -771

	err := refusedSurveyPage(t, caveatSurveyOf(selection))
	if !errors.Is(err, ErrPullRequestSurveyPageInvalid) {
		t.Fatalf("a selection at a negative run was not refused: %v", err)
	}
}

// The survey writes a reason exactly when it refuses a candidate. Selecting
// one and refusing it in the same breath is the document contradicting
// itself, and the page would print the candidate among the selections with
// its refusal dropped.
func TestASelectionCarryingARefusalIsRefused(t *testing.T) {
	selection := caveatSelection(1589, answeredSurveyHead)
	selection.Reason = "no run for Checks on " + answeredSurveyHead

	err := refusedSurveyPage(t, caveatSurveyOf(selection))
	if !errors.Is(err, ErrPullRequestSurveyPageInvalid) {
		t.Fatalf("a selection carrying a refusal was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "#1589 is selectable and is refused as no run for Checks") {
		t.Fatalf("the refusal does not carry the survey's own words: %v", err)
	}
}

// The refusal is read before the missing run, because it explains it: a
// candidate the survey refused has no run by construction, and reporting the
// missing run would send the reader after a run that was never meant to exist.
func TestARefusedSelectionIsReadBeforeItsMissingRun(t *testing.T) {
	selection := caveatSelection(1589, answeredSurveyHead)
	selection.RunID = 0
	selection.Reason = "no run for Checks on " + answeredSurveyHead

	err := refusedSurveyPage(t, caveatSurveyOf(selection))
	if !strings.Contains(err.Error(), "is refused as") {
		t.Fatalf("the missing run was read before the refusal: %v", err)
	}
}

// The other run fields are what a real run can come back with. A survey is
// not refused over them, and the page states them as they stand.
func TestASelectionWithNoConclusionIsStated(t *testing.T) {
	selection := caveatSelection(1589, answeredSurveyHead)
	selection.Attempt = 0
	selection.Event = ""
	selection.Conclusion = ""

	page := renderCaveatSurvey(t, caveatSurveyOf(selection))
	if !strings.Contains(page, "selected: #1589 at "+answeredSurveyHead+", run 10589 attempt 0") {
		t.Fatalf("an ungraded selection lost its line:\n%s", page)
	}
}

// The answers are read after the listing itself: a candidate answered for
// twice is that repeat, not two wrong answers.
func TestARepeatedCandidateIsReadBeforeItsAnswer(t *testing.T) {
	selection := caveatSelection(1589, answeredSurveyHead)
	selection.RunID = 0

	report := caveatSurveyOf(selection, selection)
	report.Considered = 2
	report.Selectable = 2

	err := refusedSurveyPage(t, report)
	if !strings.Contains(err.Error(), "answered for more than once") {
		t.Fatalf("the answer was read before the listing: %v", err)
	}
}

// The answers are read before the shared heads, so a clash between two
// candidates the survey cannot answer for is reported as the answer it got
// wrong rather than as a commit.
func TestTheAnswersAreReadBeforeTheSharedHeads(t *testing.T) {
	first := caveatSelection(1589, answeredSurveyHead)
	second := caveatSelection(1590, answeredSurveyHead)
	second.RunID = 0

	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: answeredSurveyHead, PullRequests: []int{1589, 1590, 1591}}},
		first, second,
	))
	if !strings.Contains(err.Error(), "#1590 is selectable and names no run") {
		t.Fatalf("the shared heads were read before the answers: %v", err)
	}
}

// An ordinary survey, both halves of it, is not refused by any of this.
func TestASurveyThatAnswersForEveryCandidateIsNotRefused(t *testing.T) {
	page := renderCaveatSurvey(t, caveatSurveyOf(
		caveatSelection(1589, "1111111111111111111111111111111111111111"),
		droppedSelection(1591, answeredSurveyHead),
	))
	if !strings.Contains(page, "selected: #1589 at ") {
		t.Fatalf("the selection lost its line:\n%s", page)
	}
	if !strings.Contains(page, "no evidence run: #1591 at "+answeredSurveyHead+" (no run for Checks") {
		t.Fatalf("the dropped candidate lost its reason:\n%s", page)
	}
}
