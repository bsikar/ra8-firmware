// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

// baseSelection is an ordinary selectable candidate aimed at the base given.
// Each test changes only the base it is about.
func baseSelection(number int, head, base string) surveyedPullRequest {
	candidate := caveatSelection(number, head)
	candidate.BaseRef = base
	return candidate
}

func TestASetAimedAtOneBaseSaysNothingAboutIt(t *testing.T) {
	page := renderCaveatSurvey(t, caveatSurveyOf(
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
		baseSelection(1590, "2222222222222222222222222222222222222222", "ra8ci/dev"),
	))

	if strings.Contains(page, "base ") {
		t.Fatalf("a set aimed at one base states a base:\n%s", page)
	}
}

func TestASetSpreadAcrossTwoBasesNamesBoth(t *testing.T) {
	page := renderCaveatSurvey(t, caveatSurveyOf(
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
		baseSelection(1590, "2222222222222222222222222222222222222222", "release/1.4"),
		baseSelection(1591, "3333333333333333333333333333333333333333", "ra8ci/dev"),
	))

	for _, want := range []string{
		"base ra8ci/dev: #1589, #1591\n",
		"base release/1.4: #1590\n",
	} {
		if !strings.Contains(page, want) {
			t.Fatalf("a spread set does not carry %q:\n%s", want, page)
		}
	}
}

// The bases are stated in the order the survey walked them, which is the
// order they were asked about, never alphabetically: two pages of one set
// would otherwise disagree about which candidate came first.
func TestTheBasesAreStatedInTheSurveysOwnOrder(t *testing.T) {
	page := renderCaveatSurvey(t, caveatSurveyOf(
		baseSelection(1590, "2222222222222222222222222222222222222222", "release/1.4"),
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
	))

	release := strings.Index(page, "base release/1.4:")
	dev := strings.Index(page, "base ra8ci/dev:")
	if release < 0 || dev < 0 {
		t.Fatalf("a spread set does not name both bases:\n%s", page)
	}
	if release > dev {
		t.Fatalf("the bases are not in the survey's own order:\n%s", page)
	}
}

// The bases the page states are the bases of the set that would be gathered.
// An unselectable candidate is not going to be gathered, so where it was
// aimed cannot spread the gathered set across two branches.
func TestAnUnselectableCandidateDoesNotSpreadTheSet(t *testing.T) {
	elsewhere := baseSelection(1592, "4444444444444444444444444444444444444444", "release/1.4")
	elsewhere.Selectable = false
	elsewhere.RunID = 0
	elsewhere.Attempt = 0
	elsewhere.Event = ""
	elsewhere.Conclusion = ""
	elsewhere.Reason = "no run for Checks on 4444444444444444444444444444444444444444"

	page := renderCaveatSurvey(t, caveatSurveyOf(
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
		elsewhere,
	))

	if strings.Contains(page, "base ") {
		t.Fatalf("an unselectable candidate spread the set:\n%s", page)
	}
	if !strings.Contains(page, "no evidence run: #1592") {
		t.Fatalf("the unselectable candidate lost its own line:\n%s", page)
	}
}

// A base the survey could not answer for is not a base of its own: two
// unanswered bases are not two branches.
//
// This is read off surveyedBases rather than off a page, because the page no
// longer renders one: checkSurveyedBase refuses a selection aimed at no base
// before the grouping is ever reached. The grouping rule is still the rule,
// and this is the level it is now readable at. The page's own answer to a
// blank base is pinned in pull_request_survey_base_test.go.
func TestACandidateWithNoBaseIsNotABaseOfItsOwn(t *testing.T) {
	bases := surveyedBases([]surveyedPullRequest{
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
		baseSelection(1593, "5555555555555555555555555555555555555555", "   "),
		baseSelection(1594, "6666666666666666666666666666666666666666", ""),
	})

	if len(bases) != 0 {
		t.Fatalf("two unanswered bases read as branches: %+v", bases)
	}
}

// A base is matched the way a commit is, without its casing or surrounding
// space, and is reported as the first candidate stated it.
func TestABaseIsMatchedWithoutItsCasingOrSpace(t *testing.T) {
	page := renderCaveatSurvey(t, caveatSurveyOf(
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
		baseSelection(1590, "2222222222222222222222222222222222222222", "  RA8CI/Dev  "),
	))

	if strings.Contains(page, "base ") {
		t.Fatalf("one base written two ways read as two bases:\n%s", page)
	}
}

// Naming the spread moves no verdict and no count, the rule #1625 settled
// and #1635 kept: such a set is gatherable and every candidate is still
// selected.
func TestNamingMoreThanOneBaseMovesNoVerdictAndNoCount(t *testing.T) {
	page := renderCaveatSurvey(t, caveatSurveyOf(
		baseSelection(1589, "1111111111111111111111111111111111111111", "ra8ci/dev"),
		baseSelection(1590, "2222222222222222222222222222222222222222", "release/1.4"),
	))

	lines := strings.Split(strings.TrimSuffix(page, "\n"), "\n")
	if !strings.HasPrefix(lines[0], "ready: ") {
		t.Fatalf("the verdict moved: %q", lines[0])
	}
	if lines[1] != "2 candidates considered, 2 selectable, 0 unselectable" {
		t.Fatalf("the counts moved: %q", lines[1])
	}
	for _, want := range []string{"selected: #1589 at ", "selected: #1590 at "} {
		if !strings.Contains(page, want) {
			t.Fatalf("a spread candidate lost its selection line:\n%s", page)
		}
	}
}

// Decision order: the clash that refuses the gather, then the shape of the
// set, then the candidates to drop, then the ones to read twice, then the
// set to hand on.
func TestTheBasesAreStatedBetweenTheSharedHeadsAndTheRefusals(t *testing.T) {
	shared := "6666666666666666666666666666666666666666"
	first := baseSelection(1589, shared, "ra8ci/dev")
	second := baseSelection(1590, shared, "ra8ci/dev")
	elsewhere := baseSelection(1591, "7777777777777777777777777777777777777777", "release/1.4")
	missing := baseSelection(1592, "8888888888888888888888888888888888888888", "ra8ci/dev")
	missing.Selectable = false
	missing.RunID = 0
	missing.Attempt = 0
	missing.Event = ""
	missing.Conclusion = ""
	missing.Reason = "no run for Checks on 8888888888888888888888888888888888888888"

	report := caveatSurveyOf(first, second, elsewhere, missing)
	report.SharedHeads = []surveySharedHead{{HeadSHA: shared, PullRequests: []int{1589, 1590}}}

	page := renderCaveatSurvey(t, report)

	head := strings.Index(page, "shared head ")
	base := strings.Index(page, "base ")
	refusal := strings.Index(page, "no evidence run: ")
	selected := strings.Index(page, "selected: ")
	if head < 0 || base < 0 || refusal < 0 || selected < 0 {
		t.Fatalf("a section is missing from the page:\n%s", page)
	}
	if !(head < base && base < refusal && refusal < selected) {
		t.Fatalf("the page is not in decision order:\n%s", page)
	}
}
