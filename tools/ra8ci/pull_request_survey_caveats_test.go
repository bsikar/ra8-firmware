// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"strings"
	"testing"
)

// caveatSurveyOf assembles a consistent survey around the candidates given,
// so each test states only the head facts it is about. The counts are
// derived here rather than written by hand: the page refuses a survey whose
// counts disagree with its candidates, and a test that trips that refusal
// would pass for the wrong reason.
func caveatSurveyOf(candidates ...surveyedPullRequest) pullRequestSurveyReport {
	report := pullRequestSurveyReport{
		Workflow:     "ra8ci",
		Considered:   len(candidates),
		SharedHeads:  []surveySharedHead{},
		PullRequests: candidates,
	}
	for _, candidate := range candidates {
		if candidate.Selectable {
			report.Selectable++
			continue
		}
		report.Unselectable++
	}
	return report
}

// caveatSelection is an ordinary selectable candidate: open, ours, with a
// run behind it. Each test changes only the head facts it is about.
func caveatSelection(number int, head string) surveyedPullRequest {
	return surveyedPullRequest{
		Number:         number,
		HeadSHA:        head,
		BaseRef:        "ra8ci/dev",
		State:          "open",
		HeadRepository: "bsikar/ra8-firmware",
		Selectable:     true,
		RunID:          int64(9000 + number),
		Attempt:        1,
		Event:          "pull_request",
		Conclusion:     "success",
	}
}

func renderCaveatSurvey(t *testing.T, report pullRequestSurveyReport) string {
	t.Helper()
	page := &bytes.Buffer{}
	if err := RenderPullRequestSurvey(page, report); err != nil {
		t.Fatalf("render the survey: %v", err)
	}
	return page.String()
}

func TestAMergedSelectionIsReadBeforeItIsGathered(t *testing.T) {
	merged := caveatSelection(1591, "1111111111111111111111111111111111111111")
	merged.State = "closed"
	merged.Merged = true

	page := renderCaveatSurvey(t, caveatSurveyOf(merged))

	want := "read before gathering: #1591 at 1111111111111111111111111111111111111111 (already merged)\n"
	if !strings.Contains(page, want) {
		t.Fatalf("a merged selection is not read before it is gathered:\n%s", page)
	}
	// Merged is said instead of the state, never beside it.
	if strings.Contains(page, "no longer open") {
		t.Fatalf("a merged selection states its closure twice:\n%s", page)
	}
}

func TestASelectionThatIsNoLongerOpenIsReadWithItsState(t *testing.T) {
	closed := caveatSelection(1592, "2222222222222222222222222222222222222222")
	closed.State = "closed"

	page := renderCaveatSurvey(t, caveatSurveyOf(closed))

	want := "read before gathering: #1592 at 2222222222222222222222222222222222222222 (no longer open (closed))\n"
	if !strings.Contains(page, want) {
		t.Fatalf("a closed selection is not read with its state:\n%s", page)
	}
}

func TestAForkedSelectionIsReadWithTheForkItIsIn(t *testing.T) {
	forked := caveatSelection(1593, "3333333333333333333333333333333333333333")
	forked.FromFork = true
	forked.HeadRepository = "someone/ra8-firmware"

	page := renderCaveatSurvey(t, caveatSurveyOf(forked))

	want := "read before gathering: #1593 at 3333333333333333333333333333333333333333 (from the fork someone/ra8-firmware)\n"
	if !strings.Contains(page, want) {
		t.Fatalf("a forked selection is not read with its fork:\n%s", page)
	}
}

func TestAForkedSelectionIsStillReadWhenItsForkIsNotNamed(t *testing.T) {
	forked := caveatSelection(1594, "4444444444444444444444444444444444444444")
	forked.FromFork = true
	forked.HeadRepository = ""

	page := renderCaveatSurvey(t, caveatSurveyOf(forked))

	want := "read before gathering: #1594 at 4444444444444444444444444444444444444444 (from a fork)\n"
	if !strings.Contains(page, want) {
		t.Fatalf("an unnamed fork drops the selection from the page:\n%s", page)
	}
}

func TestASelectionThatIsBothMergedAndForkedIsReadOnce(t *testing.T) {
	both := caveatSelection(1595, "5555555555555555555555555555555555555555")
	both.State = "closed"
	both.Merged = true
	both.FromFork = true
	both.HeadRepository = "someone/ra8-firmware"

	page := renderCaveatSurvey(t, caveatSurveyOf(both))

	want := "read before gathering: #1595 at 5555555555555555555555555555555555555555 (already merged; from the fork someone/ra8-firmware)\n"
	if !strings.Contains(page, want) {
		t.Fatalf("a merged fork is not read as one candidate:\n%s", page)
	}
	if strings.Count(page, "read before gathering: #1595") != 1 {
		t.Fatalf("one pull request is read as two:\n%s", page)
	}
}

func TestAnOrdinaryOpenSelectionIsNotSingledOut(t *testing.T) {
	page := renderCaveatSurvey(t, caveatSurveyOf(
		caveatSelection(1596, "6666666666666666666666666666666666666666"),
		caveatSelection(1597, "7777777777777777777777777777777777777777"),
	))

	if strings.Contains(page, "read before gathering") {
		t.Fatalf("an ordinary survey carries a caveat section:\n%s", page)
	}
}

func TestAnUnselectableCandidateIsNotReadAsASelection(t *testing.T) {
	dropped := surveyedPullRequest{
		Number:         1598,
		HeadSHA:        "8888888888888888888888888888888888888888",
		BaseRef:        "ra8ci/dev",
		State:          "closed",
		Merged:         true,
		FromFork:       true,
		HeadRepository: "someone/ra8-firmware",
		Reason:         "no ra8ci run on the head",
	}

	page := renderCaveatSurvey(t, caveatSurveyOf(dropped))

	if strings.Contains(page, "read before gathering") {
		t.Fatalf("a candidate that will not be gathered is read as one:\n%s", page)
	}
	if !strings.Contains(page, "no evidence run: #1598") {
		t.Fatalf("the unselectable candidate lost its own line:\n%s", page)
	}
}

func TestReadingASelectionMovesNoVerdictAndNoCount(t *testing.T) {
	merged := caveatSelection(1599, "9999999999999999999999999999999999999999")
	merged.State = "closed"
	merged.Merged = true

	page := renderCaveatSurvey(t, caveatSurveyOf(merged))

	if !strings.HasPrefix(page, "ready: every candidate can carry ra8ci evidence on a commit of its own\n") {
		t.Fatalf("a caveat moved the verdict:\n%s", page)
	}
	if !strings.Contains(page, "1 candidate considered, 1 selectable, 0 unselectable\n") {
		t.Fatalf("a caveat moved the counts:\n%s", page)
	}
	if !strings.Contains(page, "selected: #1599 at 9999999999999999999999999999999999999999, run 10599 attempt 1 (pull_request: success)\n") {
		t.Fatalf("a caveat took the selection out of the set:\n%s", page)
	}
}

func TestACaveatIsReadBeforeTheSelectionItIsAbout(t *testing.T) {
	merged := caveatSelection(1600, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
	merged.State = "closed"
	merged.Merged = true

	page := renderCaveatSurvey(t, caveatSurveyOf(merged))

	caveat := strings.Index(page, "read before gathering: #1600")
	selection := strings.Index(page, "selected: #1600")
	if caveat < 0 || selection < 0 {
		t.Fatalf("the page is missing one of the two lines:\n%s", page)
	}
	if caveat > selection {
		t.Fatalf("the caveat is read after the selection it is about:\n%s", page)
	}
}
