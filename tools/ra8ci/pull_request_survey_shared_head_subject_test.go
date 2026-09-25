// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

// sharedCommitHead is the commit the groups below are about, written once so
// a test that is not about the spelling does not carry one.
const sharedCommitHead = "8888888888888888888888888888888888888888"

// A survey the command itself made is never refused: sharedHeads groups by
// the commit it read off the candidates, so a group without one cannot come
// out of a survey of ours.
func TestACandidateSurveysOwnSharedCommitIsNotRefused(t *testing.T) {
	page := renderCaveatSurvey(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: sharedCommitHead, PullRequests: []int{1589, 1590}}},
		baseSelection(1589, sharedCommitHead, "ra8ci/dev"),
		baseSelection(1590, sharedCommitHead, "ra8ci/dev"),
	))
	if !strings.Contains(page, "shared head "+sharedCommitHead+": #1589, #1590") {
		t.Fatalf("the survey's own shared head is not on the page:\n%s", page)
	}
}

// The line this section leads the page with is "shared head <commit>: #1589,
// #1590", and without the commit it is a clash an operator cannot go and
// look at.
func TestASharedHeadThatNamesNoCommitIsRefused(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{PullRequests: []int{1589, 1590}}},
		baseSelection(1589, sharedCommitHead, "ra8ci/dev"),
		baseSelection(1590, sharedCommitHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "names no commit") {
		t.Fatalf("the refusal does not say what is missing: %v", err)
	}
}

// Whitespace is not a statement, the rule the rest of this page keeps for a
// commit: a head of spaces prints as a blank one.
func TestASharedHeadOfSpacesNamesNoCommit(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: "   ", PullRequests: []int{1589, 1590}}},
		baseSelection(1589, sharedCommitHead, "ra8ci/dev"),
		baseSelection(1590, sharedCommitHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "names no commit") {
		t.Fatalf("a head of spaces was read as a commit: %v", err)
	}
}

// The commit is read before the candidates the group names, because the
// refusal about those candidates prints the commit: an unsurveyed candidate
// under a blank commit would be refused as "#9999 shares  and was not
// surveyed", a sentence with a gap where the subject goes.
func TestTheSharedCommitIsReadBeforeTheCandidatesItGroups(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{PullRequests: []int{1589, 9999}}},
		baseSelection(1589, sharedCommitHead, "ra8ci/dev"),
		baseSelection(1590, sharedCommitHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "names no commit") {
		t.Fatalf("the group was read for its candidates before its commit: %v", err)
	}
	if strings.Contains(err.Error(), "not surveyed") {
		t.Fatalf("the refusal names a candidate under a commit it cannot print: %v", err)
	}
}

// It is read before the arity too, for the same reason: "  is shared by 1
// candidate(s)" states the count over nothing.
func TestTheSharedCommitIsReadBeforeTheCountOfCandidates(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{PullRequests: []int{1589}}},
		baseSelection(1589, sharedCommitHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "names no commit") {
		t.Fatalf("the group was read for its arity before its commit: %v", err)
	}
	if strings.Contains(err.Error(), "shared by 1") {
		t.Fatalf("the refusal states a count over a commit it cannot print: %v", err)
	}
}

// The refusal says which group it is by the number of candidates under it,
// never by naming them: a group that carries no commit may carry no
// candidates either, and the count renders on every shape.
func TestAnEmptySharedHeadIsRefusedForItsCommitFirst(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{}},
		baseSelection(1589, sharedCommitHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "over 0 candidate(s) names no commit") {
		t.Fatalf("the refusal does not render on a group that names nothing: %v", err)
	}
}

// Two groups that both name no commit are refused as that rather than as one
// commit grouped twice, which is what the lowercased empty key would have
// made of them.
func TestTwoSharedHeadsWithNoCommitAreRefusedAsUnnamed(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{
			{PullRequests: []int{1589, 1590}},
			{PullRequests: []int{1589, 1590}},
		},
		baseSelection(1589, sharedCommitHead, "ra8ci/dev"),
		baseSelection(1590, sharedCommitHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "names no commit") {
		t.Fatalf("two unnamed groups were read as one commit twice: %v", err)
	}
	if strings.Contains(err.Error(), "more than one group") {
		t.Fatalf("the refusal states a repeat over a commit it cannot print: %v", err)
	}
}
