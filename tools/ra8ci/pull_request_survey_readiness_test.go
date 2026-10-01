// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

// readinessSurveyHead is the commit the candidates in this file sit on unless
// a test is about a second one.
const readinessSurveyHead = "dddddddddddddddddddddddddddddddddddddddd"

// readsAsReady reports how the page opened: the first line is the verdict,
// and it is the line an operator reads before deciding to run the gather.
func readsAsReady(t *testing.T, page string) bool {
	t.Helper()
	switch {
	case strings.HasPrefix(page, "ready: "):
		return true
	case strings.HasPrefix(page, "not ready: "):
		return false
	}
	t.Fatalf("the page does not open with a verdict:\n%s", page)
	return false
}

// The page and the exit status answer one question, so they cannot come
// apart. Every shape a survey takes is read both ways here: a third reason a
// set cannot be gathered would otherwise have to be remembered in two places.
func TestThePageAndTheVerdictAgreeOnEverySurvey(t *testing.T) {
	shared := "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	sharing := caveatSurveyOf(
		caveatSelection(1589, shared),
		caveatSelection(1590, shared),
	)
	sharing.SharedHeads = []surveySharedHead{{HeadSHA: shared, PullRequests: []int{1589, 1590}}}

	dropped := caveatSurveyOf(
		caveatSelection(1589, readinessSurveyHead),
		droppedSelection(1591, "ffffffffffffffffffffffffffffffffffffffff"),
	)

	both := caveatSurveyOf(
		caveatSelection(1589, shared),
		caveatSelection(1590, shared),
		droppedSelection(1591, "ffffffffffffffffffffffffffffffffffffffff"),
	)
	both.SharedHeads = []surveySharedHead{{HeadSHA: shared, PullRequests: []int{1589, 1590}}}

	for _, survey := range []struct {
		as     string
		report pullRequestSurveyReport
	}{
		{"a set with nothing in it", caveatSurveyOf()},
		{"a set every candidate can carry", caveatSurveyOf(
			caveatSelection(1589, readinessSurveyHead),
			caveatSelection(1590, "1111111111111111111111111111111111111111"),
		)},
		{"a set two candidates share a commit in", sharing},
		{"a set with a candidate no run can be selected on", dropped},
		{"a set with one of each", both},
	} {
		page := renderCaveatSurvey(t, survey.report)
		stated := readsAsReady(t, page)
		refused := candidateSurveyVerdict(survey.report) != nil
		if stated == refused {
			t.Fatalf("%s reads as ready=%t and is refused=%t:\n%s",
				survey.as, stated, refused, page)
		}
	}
}

// A shared head refuses the gather on its own: both candidates are
// selectable, both are counted so, and the page must not open "ready" over a
// command that exits non-zero.
func TestASharedCommitAloneIsNotReadyOnEitherAnswer(t *testing.T) {
	shared := "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	report := caveatSurveyOf(
		caveatSelection(1589, shared),
		caveatSelection(1590, shared),
	)
	report.SharedHeads = []surveySharedHead{{HeadSHA: shared, PullRequests: []int{1589, 1590}}}

	if candidateSetIsReady(report) {
		t.Fatal("a set two candidates share a commit in reads as ready")
	}
	if report.Unselectable != 0 {
		t.Fatalf("the shared set is refused for the wrong reason: %d unselectable", report.Unselectable)
	}
	err := candidateSurveyVerdict(report)
	if err == nil || strings.Contains(err.Error(), "no ra8ci run") {
		t.Fatalf("the verdict does not name the shared commit: %v", err)
	}
}

// An empty survey is a real, empty answer and both halves state it as one.
func TestASurveyOfNoCandidatesIsReadyOnBothAnswers(t *testing.T) {
	report := caveatSurveyOf()
	if !candidateSetIsReady(report) {
		t.Fatal("a survey of no candidates does not read as ready")
	}
	if err := candidateSurveyVerdict(report); err != nil {
		t.Fatalf("a survey of no candidates was refused: %v", err)
	}
	if !readsAsReady(t, renderCaveatSurvey(t, report)) {
		t.Fatal("the page does not state an empty survey as ready")
	}
}
