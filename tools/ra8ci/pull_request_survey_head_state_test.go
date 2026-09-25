// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

const headStateSurveyHead = "2222222222222222222222222222222222222222"

// A selection stating no head state is refused. The page says nothing at all
// about a candidate with no caveat, so this one would render as open, ours
// and ordinary: there is not even a gap for a reader to notice.
func TestASelectionInNoStateIsRefused(t *testing.T) {
	stateless := caveatSelection(1591, headStateSurveyHead)
	stateless.State = ""

	err := refusedSurveyPage(t, caveatSurveyOf(stateless))
	if !strings.Contains(err.Error(), "#1591 is selectable and is in no state") {
		t.Fatalf("err = %v, does not say the candidate is in no state", err)
	}
}

// Whitespace is not a statement, the rule the other checks on this page keep
// for a commit.
func TestASelectionWhoseStateIsSpaceIsRefused(t *testing.T) {
	stateless := caveatSelection(1591, headStateSurveyHead)
	stateless.State = "   "

	err := refusedSurveyPage(t, caveatSurveyOf(stateless))
	if !strings.Contains(err.Error(), "is in no state") {
		t.Fatalf("err = %v, read whitespace as a state", err)
	}
}

// A selection that is merged and stated open is refused. Both cannot be
// true, and caveatedSelections says "already merged" and drops the state, so
// the contradiction reaches the page by neither route.
func TestAMergedSelectionStatedOpenIsRefused(t *testing.T) {
	merged := caveatSelection(1592, headStateSurveyHead)
	merged.Merged = true

	err := refusedSurveyPage(t, caveatSurveyOf(merged))
	if !strings.Contains(err.Error(), "#1592 is merged and is stated open") {
		t.Fatalf("err = %v, does not name the contradiction", err)
	}
}

// The state is matched the way GitHub's word for it is written, without its
// casing: "OPEN" over a merged pull request is the same contradiction.
func TestAMergedSelectionStatedOpenInAnyCasingIsRefused(t *testing.T) {
	merged := caveatSelection(1592, headStateSurveyHead)
	merged.Merged = true
	merged.State = "OPEN"

	err := refusedSurveyPage(t, caveatSurveyOf(merged))
	if !strings.Contains(err.Error(), "is merged and is stated OPEN") {
		t.Fatalf("err = %v, read the casing as a different state", err)
	}
}

// The blank is read first: a state that was never stated cannot contradict
// anything.
func TestAMergedSelectionInNoStateIsRefusedForItsStateFirst(t *testing.T) {
	merged := caveatSelection(1592, headStateSurveyHead)
	merged.Merged = true
	merged.State = ""

	err := refusedSurveyPage(t, caveatSurveyOf(merged))
	if !strings.Contains(err.Error(), "is in no state") {
		t.Fatalf("err = %v, want the missing state", err)
	}
	if strings.Contains(err.Error(), "is merged and is stated") {
		t.Fatalf("err = %v, refused as a contradiction rather than as no state", err)
	}
}

// Only the selections are read. An unselectable candidate is named on its
// own line, is not going to be gathered, and none of its head facts are
// printed, so a state it does not answer for takes no readable page away.
func TestAnUnselectableCandidateInNoStateIsStated(t *testing.T) {
	dropped := caveatSelection(1593, headStateSurveyHead)
	dropped.Selectable = false
	dropped.RunID = 0
	dropped.Attempt = 0
	dropped.Event = ""
	dropped.Conclusion = ""
	dropped.Reason = "no Checks run on " + headStateSurveyHead
	dropped.State = ""

	page := renderCaveatSurvey(t, caveatSurveyOf(dropped))
	if !strings.Contains(page, "no evidence run: #1593 at "+headStateSurveyHead) {
		t.Fatalf("page = %q, does not state the dropped candidate", page)
	}
}

// The state is read AFTER the answers: a selectable candidate carrying a
// refusal is about what the listing answers, and is refused as that whatever
// state it is in.
func TestARefusedSelectionIsReadBeforeItsState(t *testing.T) {
	contradictory := caveatSelection(1594, headStateSurveyHead)
	contradictory.State = ""
	contradictory.Reason = "no Checks run on " + headStateSurveyHead

	err := refusedSurveyPage(t, caveatSurveyOf(contradictory))
	if !strings.Contains(err.Error(), "is selectable and is refused as") {
		t.Fatalf("err = %v, want the answers refusal", err)
	}
}

// The state is read BEFORE the shared heads, the order #1648 settled for the
// head commit: a clash is read in the light of candidates the page can state.
func TestTheStateIsReadBeforeTheSharedHeads(t *testing.T) {
	first := caveatSelection(1595, headStateSurveyHead)
	first.State = ""
	second := caveatSelection(1596, headStateSurveyHead)
	report := caveatSurveyOf(first, second)
	report.SharedHeads = []surveySharedHead{{
		HeadSHA:      headStateSurveyHead,
		PullRequests: []int{1595, 1596},
	}}

	err := refusedSurveyPage(t, report)
	if !strings.Contains(err.Error(), "#1595 is selectable and is in no state") {
		t.Fatalf("err = %v, want the state refusal", err)
	}
	if strings.Contains(err.Error(), "shares") {
		t.Fatalf("err = %v, refused as a shared head rather than as the state", err)
	}
}

// A merged selection stated closed is the ordinary merged candidate, and it
// is still read before it is gathered.
func TestAMergedSelectionStatedClosedIsStated(t *testing.T) {
	merged := caveatSelection(1597, headStateSurveyHead)
	merged.Merged = true
	merged.State = "closed"

	page := renderCaveatSurvey(t, caveatSurveyOf(merged))
	if !strings.Contains(page, "read before gathering: #1597 at "+headStateSurveyHead+" (already merged)") {
		t.Fatalf("page = %q, does not read the merged selection before it is gathered", page)
	}
}
