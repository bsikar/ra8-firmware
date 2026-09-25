// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

const basedSurveyHead = "5555555555555555555555555555555555555555"

// A selection aimed at no base is refused. surveyedBases skips it by the key
// it groups on, so the candidate is absent from the grouping entirely: the
// page names the bases of the rest of the set and says nothing at all about
// where this one is aimed.
func TestASelectionAimedAtNoBaseIsRefused(t *testing.T) {
	aimless := caveatSelection(1591, basedSurveyHead)
	aimless.BaseRef = ""

	err := refusedSurveyPage(t, caveatSurveyOf(aimless))
	if !strings.Contains(err.Error(), "#1591 is selectable and is aimed at no base") {
		t.Fatalf("err = %v, does not say the candidate is aimed at no base", err)
	}
}

// Whitespace is not a statement, the rule every other check on this page
// keeps: surveyedBases trims before it groups, so a base of three spaces is
// skipped exactly like a blank one.
func TestASelectionWhoseBaseIsSpaceIsRefused(t *testing.T) {
	aimless := caveatSelection(1591, basedSurveyHead)
	aimless.BaseRef = "   "

	err := refusedSurveyPage(t, caveatSurveyOf(aimless))
	if !strings.Contains(err.Error(), "is aimed at no base") {
		t.Fatalf("err = %v, read whitespace as a base", err)
	}
}

// *** THE SHAPE THIS CHECK EXISTS FOR. Two selections on one base and a
// third aimed at nothing leave one key in surveyedBases, the section is
// omitted for being a set aimed at one base, and a page whose whole job is
// to say the set is spread says the opposite by saying nothing. Without the
// refusal this renders cleanly. ***
func TestASetWhoseSpreadIsHiddenByAnUnaimedSelectionIsRefused(t *testing.T) {
	aimless := caveatSelection(1591, "3333333333333333333333333333333333333333")
	aimless.BaseRef = ""

	err := refusedSurveyPage(t, caveatSurveyOf(
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
		baseSelection(1590, "2222222222222222222222222222222222222222", "ra8ci/dev"),
		aimless,
	))
	if !strings.Contains(err.Error(), "#1591 is selectable and is aimed at no base") {
		t.Fatalf("err = %v, does not name the candidate that hid the spread", err)
	}
}

// Only the selections are read, the rule checkSurveyedHeadState keeps for
// the head facts and for the same reason: the grouping is derived from the
// selections alone, and an unselectable candidate is dropped from it
// deliberately, so where it was aimed is printed nowhere.
func TestAnUnselectableCandidateAimedAtNoBaseIsStated(t *testing.T) {
	aimless := caveatSelection(1592, "4444444444444444444444444444444444444444")
	aimless.BaseRef = ""
	aimless.Selectable = false
	aimless.RunID = 0
	aimless.Attempt = 0
	aimless.Event = ""
	aimless.Conclusion = ""
	aimless.Reason = "no run for Checks on 4444444444444444444444444444444444444444"

	page := renderCaveatSurvey(t, caveatSurveyOf(
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
		aimless,
	))
	if !strings.Contains(page, "no evidence run: #1592 at 4444444444444444444444444444444444444444") {
		t.Fatalf("an unselectable candidate aimed at no base is not stated:\n%s", page)
	}
}

// The head state is read first, so a selection that is merged and aimed at
// nothing is refused as the contradiction the caveat section would have
// stated rather than for a base nobody would have read on that page.
func TestTheHeadStateIsReadBeforeTheBase(t *testing.T) {
	merged := caveatSelection(1592, basedSurveyHead)
	merged.Merged = true
	merged.BaseRef = ""

	err := refusedSurveyPage(t, caveatSurveyOf(merged))
	if !strings.Contains(err.Error(), "#1592 is merged and is stated open") {
		t.Fatalf("err = %v, did not read the head state first", err)
	}
	if strings.Contains(err.Error(), "aimed at no base") {
		t.Fatalf("err = %v, read the base before the head state", err)
	}
}

// And the base is read before the shared heads, so a grouping refusal never
// names a candidate this page could not have placed.
func TestTheBaseIsReadBeforeTheSharedHeads(t *testing.T) {
	first := caveatSelection(1589, basedSurveyHead)
	first.BaseRef = ""
	second := caveatSelection(1590, basedSurveyHead)

	report := caveatSurveyOf(first, second)
	report.SharedHeads = []surveySharedHead{{
		HeadSHA:      basedSurveyHead,
		PullRequests: []int{1589, 1590},
	}}

	err := refusedSurveyPage(t, report)
	if !strings.Contains(err.Error(), "#1589 is selectable and is aimed at no base") {
		t.Fatalf("err = %v, does not read the base first", err)
	}
	if strings.Contains(err.Error(), "shares") {
		t.Fatalf("err = %v, read the shared heads before the base", err)
	}
}

// Nothing a real survey writes is refused: an ordinary spread set still
// renders, and the bases it names are unchanged by this check.
func TestAnOrdinarySpreadSetStillStatesItsBases(t *testing.T) {
	page := renderCaveatSurvey(t, caveatSurveyOf(
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
		baseSelection(1590, "2222222222222222222222222222222222222222", "release/1.4"),
	))
	for _, want := range []string{
		"base ra8ci/dev: #1589\n",
		"base release/1.4: #1590\n",
	} {
		if !strings.Contains(page, want) {
			t.Fatalf("a spread set does not carry %q:\n%s", want, page)
		}
	}
}
