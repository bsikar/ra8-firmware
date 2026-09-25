// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

const sharedClashHead = "7777777777777777777777777777777777777777"

// The readiness line is read off the grouping, so a clash the grouping does
// not carry is a clean verdict over a set the gather refuses.
func TestACommitTwoCandidatesShareAndNothingGroupsIsRefused(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(nil,
		caveatSelection(1589, sharedClashHead),
		caveatSelection(1590, sharedClashHead),
	))
	if !strings.Contains(err.Error(), "#1589 and #1590") {
		t.Fatalf("the refusal does not name the candidates: %v", err)
	}
	if !strings.Contains(err.Error(), sharedClashHead) {
		t.Fatalf("the refusal does not name the commit: %v", err)
	}
	if !strings.Contains(err.Error(), "not grouped") {
		t.Fatalf("the refusal does not say what is missing: %v", err)
	}
}

// An unselectable candidate is read too: sharedHeads walks the surveyed
// heads, not the selections, and a clash between a selection and a dropped
// candidate is still a commit counted twice.
func TestAClashWithAnUnselectableCandidateIsRefused(t *testing.T) {
	dropped := droppedSelection(1591, sharedClashHead)
	err := refusedSurveyPage(t, sharedHeadSurveyOf(nil,
		caveatSelection(1589, sharedClashHead),
		dropped,
	))
	if !strings.Contains(err.Error(), "not grouped") {
		t.Fatalf("the refusal does not say what is missing: %v", err)
	}
}

// A group that names some of the candidates at its commit understates the
// clash on the one line a reader uses to size it.
func TestACandidateLeftOutOfItsSharedHeadIsRefused(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: sharedClashHead, PullRequests: []int{1589, 1590}}},
		caveatSelection(1589, sharedClashHead),
		caveatSelection(1590, sharedClashHead),
		caveatSelection(1591, sharedClashHead),
	))
	if !strings.Contains(err.Error(), "#1591 is at") {
		t.Fatalf("the refusal does not name the candidate left out: %v", err)
	}
	if !strings.Contains(err.Error(), "not named among the candidates sharing it") {
		t.Fatalf("the refusal does not say what is missing: %v", err)
	}
}

// One commit written two ways is one commit, the rule sharedHeads groups by,
// so a group stated in another casing is the group for it.
func TestASharedHeadGroupedInAnotherCasingIsTheGroupForIt(t *testing.T) {
	head := "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
	page := renderCaveatSurvey(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: strings.ToLower(head), PullRequests: []int{1589, 1590}}},
		caveatSelection(1589, head),
		caveatSelection(1590, head),
	))
	if !strings.Contains(page, "shared head "+strings.ToLower(head)+": #1589, #1590") {
		t.Fatalf("the group is not stated:\n%s", page)
	}
}

// Two candidates the survey could not answer for a head on are two unanswered
// heads, not a shared commit. The blank is refused as a blank, by the check
// that reads the subject.
func TestTwoCandidatesAtNoCommitAreNotAClash(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(nil,
		caveatSelection(1589, ""),
		caveatSelection(1590, ""),
	))
	if !strings.Contains(err.Error(), "is at no commit") {
		t.Fatalf("the refusal is not the blank head one: %v", err)
	}
	if strings.Contains(err.Error(), "not grouped") {
		t.Fatalf("two unanswered heads were read as a clash: %v", err)
	}
}

// The groups are read first, so a group that is itself malformed is refused
// as that rather than as a candidate missing from it.
func TestAMalformedGroupIsReadBeforeTheCandidatesAtItsCommit(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: sharedClashHead, PullRequests: []int{1589}}},
		caveatSelection(1589, sharedClashHead),
		caveatSelection(1590, sharedClashHead),
	))
	if !strings.Contains(err.Error(), "shared by 1 candidate") {
		t.Fatalf("the refusal does not lead with the group: %v", err)
	}
	if strings.Contains(err.Error(), "not named among") {
		t.Fatalf("the refusal answers for the missing candidate as well: %v", err)
	}
}

// One candidate at a commit of its own is the ordinary case and is grouped
// nowhere.
func TestCandidatesAtCommitsOfTheirOwnAreNotGrouped(t *testing.T) {
	page := renderCaveatSurvey(t, sharedHeadSurveyOf(nil,
		caveatSelection(1589, sharedClashHead),
		caveatSelection(1590, strings.Repeat("8", 40)),
	))
	if strings.Contains(page, "shared head") {
		t.Fatalf("a set with no clash was grouped:\n%s", page)
	}
	if !strings.Contains(page, "ready: ") {
		t.Fatalf("a set with no clash is not ready:\n%s", page)
	}
}

// A survey the command itself made is never refused by this check.
func TestASurveysOwnSharedCommitsAreNotRefused(t *testing.T) {
	page := renderCaveatSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, sharedClashHead, 771, 1, "success"),
		selectableFor(1590, sharedClashHead, 772, 1, "success"),
		selectableFor(1591, strings.Repeat("9", 40), 773, 1, "success"),
	}))
	if !strings.Contains(page, "shared head "+sharedClashHead+": #1589, #1590") {
		t.Fatalf("the survey's own clash is not on the page:\n%s", page)
	}
}
