// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

// numberedShareHead is the commit the groups below are about.
const numberedShareHead = "9999999999999999999999999999999999999999"

// A survey the command itself made is never refused: the numbers in a group
// are the numbers of the candidates it grouped.
func TestASurveysOwnSharedCandidateNumbersAreNotRefused(t *testing.T) {
	page := renderCaveatSurvey(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: numberedShareHead, PullRequests: []int{1589, 1590}}},
		baseSelection(1589, numberedShareHead, "ra8ci/dev"),
		baseSelection(1590, numberedShareHead, "ra8ci/dev"),
	))
	if !strings.Contains(page, "shared head "+numberedShareHead+": #1589, #1590") {
		t.Fatalf("the survey's own shared head is not on the page:\n%s", page)
	}
}

// "#0" is not a pull request, and the page prints the group's numbers as the
// things a reader opens.
func TestACandidateSharingACommitWithNoNumberIsRefused(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: numberedShareHead, PullRequests: []int{1589, 0}}},
		baseSelection(1589, numberedShareHead, "ra8ci/dev"),
		baseSelection(1590, numberedShareHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "is numbered 0") {
		t.Fatalf("the refusal does not say the candidate carries no number: %v", err)
	}
}

// A negative number is the same document, written differently.
func TestACandidateSharingACommitWithANegativeNumberIsRefused(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: numberedShareHead, PullRequests: []int{1589, -3}}},
		baseSelection(1589, numberedShareHead, "ra8ci/dev"),
		baseSelection(1590, numberedShareHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "is numbered -3") {
		t.Fatalf("a negative number was read as a candidate: %v", err)
	}
}

// The number is read before the lookup. The listing already refuses an
// unnumbered candidate, so #0 can only ever be looked up and missed, and
// "was not surveyed" sends a reader off to find a pull request the survey
// was never asked about.
func TestAnUnnumberedShareIsReadBeforeTheLookup(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: numberedShareHead, PullRequests: []int{0, 1590}}},
		baseSelection(1589, numberedShareHead, "ra8ci/dev"),
		baseSelection(1590, numberedShareHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "is numbered 0") {
		t.Fatalf("the group was looked up before its number was read: %v", err)
	}
	if strings.Contains(err.Error(), "not surveyed") {
		t.Fatalf("an unnumbered candidate was refused as one nobody surveyed: %v", err)
	}
}

// The group's own commit is still read first: it is what every refusal in
// here prints, this one included.
func TestTheSharedCommitIsStillReadBeforeTheNumbers(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{PullRequests: []int{1589, 0}}},
		baseSelection(1589, numberedShareHead, "ra8ci/dev"),
		baseSelection(1590, numberedShareHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "names no commit") {
		t.Fatalf("the numbers were read before the commit they share: %v", err)
	}
}

// Two unnumbered shares are refused as unnumbered rather than as one
// candidate sharing a commit with itself, which is what the repeat check
// would have made of them.
func TestTwoUnnumberedSharesAreRefusedAsUnnumbered(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: numberedShareHead, PullRequests: []int{0, 0}}},
		baseSelection(1589, numberedShareHead, "ra8ci/dev"),
		baseSelection(1590, numberedShareHead, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "is numbered 0") {
		t.Fatalf("two unnumbered shares were read as one candidate twice: %v", err)
	}
	if strings.Contains(err.Error(), "with itself") {
		t.Fatalf("the refusal names a repeat over a number nobody can open: %v", err)
	}
}
