// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

// The listing is the slice every other section of the page is derived from,
// and the counts are derived from it too, so a survey answering for one pull
// request twice passes both count checks and then prints it twice.
func TestACandidateAnsweredForTwiceIsRefused(t *testing.T) {
	err := refusedSurveyPage(t, caveatSurveyOf(
		caveatSelection(1589, surveyHeadA),
		caveatSelection(1590, surveyHeadB),
		caveatSelection(1589, surveyHeadA),
	))
	if !strings.Contains(err.Error(), "#1589 is answered for more than once") {
		t.Fatalf("the refusal does not name the repeated candidate: %v", err)
	}
}

// An unselectable candidate is answered for on its own line and is read the
// same way: repeating it counts one refused pull request as two.
func TestAnUnselectableCandidateAnsweredForTwiceIsRefused(t *testing.T) {
	refused := surveyedPullRequest{
		Number: 1591, HeadSHA: surveyHeadB, BaseRef: "ra8ci/dev", State: "open",
		Reason: "no run on the head",
	}
	err := refusedSurveyPage(t, caveatSurveyOf(refused, refused))
	if !strings.Contains(err.Error(), "#1591 is answered for more than once") {
		t.Fatalf("the refusal does not name the repeated candidate: %v", err)
	}
}

// The listing is read BEFORE the shared heads, and this is the reading that
// makes the order matter: the shared-head check looks a candidate up by its
// number, so one number answered for at two commits would have it refused
// against whichever answer came last, naming a head the page never prints.
func TestACandidateAnsweredForAtTwoCommitsIsRefusedAsARepeat(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: surveyHeadA, PullRequests: []int{1589, 1590}}},
		caveatSelection(1589, surveyHeadA),
		caveatSelection(1590, surveyHeadA),
		caveatSelection(1589, surveyHeadB),
	))
	if !strings.Contains(err.Error(), "#1589 is answered for more than once") {
		t.Fatalf("the repeat was not read before the shared heads: %v", err)
	}
	if strings.Contains(err.Error(), "shares") {
		t.Fatalf("the refusal is about the grouping rather than the listing: %v", err)
	}
}

// Every line an operator acts on names a candidate by number and their next
// move is to open it. A candidate with no number is named by the commit it
// sits on, the only thing left to identify it with.
func TestAnUnnumberedCandidateIsRefused(t *testing.T) {
	err := refusedSurveyPage(t, caveatSurveyOf(
		caveatSelection(1589, surveyHeadA),
		caveatSelection(0, surveyHeadB),
	))
	if !strings.Contains(err.Error(), "a candidate at "+surveyHeadB+" is numbered 0") {
		t.Fatalf("the refusal does not name where the candidate sits: %v", err)
	}
}

// A number below zero is the same finding and is named as it was stated,
// rather than reported as missing.
func TestACandidateNumberedBelowZeroIsRefused(t *testing.T) {
	err := refusedSurveyPage(t, caveatSurveyOf(caveatSelection(-3, surveyHeadA)))
	if !strings.Contains(err.Error(), "is numbered -3") {
		t.Fatalf("the refusal does not state the number: %v", err)
	}
}

// A survey that answered for neither the number nor the head is refused
// saying so, rather than with an empty gap in the sentence.
func TestAnUnnumberedCandidateWithNoCommitIsStillNamed(t *testing.T) {
	err := refusedSurveyPage(t, caveatSurveyOf(surveyedPullRequest{
		BaseRef: "ra8ci/dev", State: "open", Reason: "no run on the head",
	}))
	if !strings.Contains(err.Error(), "a candidate at an unstated commit is numbered 0") {
		t.Fatalf("the refusal reads with a gap in it: %v", err)
	}
}

// Two candidates at one commit are the clash the page leads with, not a
// repeat: they are two pull requests and the listing answers for each once.
func TestTwoCandidatesAtOneCommitAreNotRefused(t *testing.T) {
	page := renderCaveatSurvey(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: surveyHeadA, PullRequests: []int{1589, 1590}}},
		caveatSelection(1589, surveyHeadA),
		caveatSelection(1590, surveyHeadA),
	))
	if !strings.Contains(page, "shared head "+surveyHeadA+": #1589, #1590") {
		t.Fatalf("an ordinary clash was not stated:\n%s", page)
	}
}

// A survey the command itself made is never refused: the checks are about a
// document that came from somewhere else.
func TestASurveysOwnListingIsNotRefused(t *testing.T) {
	page := renderCaveatSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "success"),
		selectableFor(1590, surveyHeadB, 772, 1, "success"),
	}))
	if !strings.Contains(page, "selected: #1589 ") || !strings.Contains(page, "selected: #1590 ") {
		t.Fatalf("the survey's own listing is not on the page:\n%s", page)
	}
}
