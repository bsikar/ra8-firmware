// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"errors"
	"strings"
	"testing"
)

// sharedHeadSurveyOf is caveatSurveyOf with the shared heads stated, so each
// test below states only the grouping it is about.
func sharedHeadSurveyOf(shared []surveySharedHead, candidates ...surveyedPullRequest) pullRequestSurveyReport {
	report := caveatSurveyOf(candidates...)
	report.SharedHeads = shared
	return report
}

// refusedSurveyPage renders and expects the refusal, and checks nothing was
// written above it: a caller handing the page to a terminal must not be left
// with half of one over an error.
func refusedSurveyPage(t *testing.T, report pullRequestSurveyReport) error {
	t.Helper()
	page := &bytes.Buffer{}
	err := RenderPullRequestSurvey(page, report)
	if err == nil {
		t.Fatalf("the survey rendered rather than being refused:\n%s", page)
	}
	if !errors.Is(err, ErrPullRequestSurveyPageInvalid) {
		t.Fatalf("the refusal is not the page's own: %v", err)
	}
	if page.Len() > 0 {
		t.Fatalf("a refused survey wrote a page:\n%s", page)
	}
	return err
}

// A survey the command itself made is never refused. The three checks are
// about a document that came from somewhere else.
func TestASurveysOwnSharedHeadsAreNotRefused(t *testing.T) {
	shared := "6666666666666666666666666666666666666666"
	page := renderCaveatSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, shared, 771, 1, "success"),
		selectableFor(1590, shared, 772, 1, "success"),
	}))
	if !strings.Contains(page, "shared head "+shared+": #1589, #1590") {
		t.Fatalf("the survey's own shared head is not on the page:\n%s", page)
	}
}

func TestASharedHeadWithOneCandidateIsRefused(t *testing.T) {
	head := "1111111111111111111111111111111111111111"
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: head, PullRequests: []int{1589}}},
		baseSelection(1589, head, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "shared by 1 candidate") {
		t.Fatalf("the refusal does not say what is wrong: %v", err)
	}
}

func TestASharedHeadNamingACandidateThatWasNotSurveyedIsRefused(t *testing.T) {
	head := "2222222222222222222222222222222222222222"
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: head, PullRequests: []int{1589, 9999}}},
		baseSelection(1589, head, "ra8ci/dev"),
		baseSelection(1590, "3333333333333333333333333333333333333333", "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "#9999") || !strings.Contains(err.Error(), "not surveyed") {
		t.Fatalf("the refusal does not name the candidate nobody surveyed: %v", err)
	}
}

func TestASharedHeadNoCandidateIsAtIsRefused(t *testing.T) {
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{
			HeadSHA:      "4444444444444444444444444444444444444444",
			PullRequests: []int{1589, 1590},
		}},
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
		baseSelection(1590, "1111111111111111111111111111111111111111", "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "is at 1111111111111111111111111111111111111111") {
		t.Fatalf("the refusal does not say where the candidate actually is: %v", err)
	}
}

func TestACandidateSharingACommitWithItselfIsRefused(t *testing.T) {
	head := "5555555555555555555555555555555555555555"
	err := refusedSurveyPage(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: head, PullRequests: []int{1589, 1589}}},
		baseSelection(1589, head, "ra8ci/dev"),
	))
	if !strings.Contains(err.Error(), "with itself") {
		t.Fatalf("the refusal does not say the candidate was named twice: %v", err)
	}
}

// A commit is matched the way the survey grouped it, without its casing or
// surrounding space. Refusing over the spelling would refuse a survey that
// is perfectly well formed.
func TestASharedHeadIsMatchedWithoutItsCasingOrSpace(t *testing.T) {
	page := renderCaveatSurvey(t, sharedHeadSurveyOf(
		[]surveySharedHead{{
			HeadSHA:      "  AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA  ",
			PullRequests: []int{1589, 1590},
		}},
		baseSelection(1589, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ra8ci/dev"),
		baseSelection(1590, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ra8ci/dev"),
	))
	if !strings.Contains(page, "shared head ") {
		t.Fatalf("one commit written two ways was dropped from the page:\n%s", page)
	}
}

// An unselectable candidate is surveyed and can share a commit, so it is
// checked against the same candidates the rest of the page reads.
func TestAnUnselectableCandidateCanShareACommit(t *testing.T) {
	head := "7777777777777777777777777777777777777777"
	missing := baseSelection(1591, head, "ra8ci/dev")
	missing.Selectable = false
	missing.RunID = 0
	missing.Attempt = 0
	missing.Event = ""
	missing.Conclusion = ""
	missing.Reason = "no run for Checks on " + head

	page := renderCaveatSurvey(t, sharedHeadSurveyOf(
		[]surveySharedHead{{HeadSHA: head, PullRequests: []int{1589, 1591}}},
		baseSelection(1589, head, "ra8ci/dev"),
		missing,
	))
	if !strings.Contains(page, "shared head "+head+": #1589, #1591") {
		t.Fatalf("an unselectable candidate was refused its share of a commit:\n%s", page)
	}
}
