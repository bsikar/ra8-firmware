// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

// A commit grouped twice is the one way the shared-head section could grow
// past the candidates it is about: each group is bounded by the candidates it
// may name, but nothing stopped a document repeating one clash under a
// thousand spellings of one commit.
func TestACommitGroupedTwiceIsRefused(t *testing.T) {
	report := sharedHeadSurveyOf(
		[]surveySharedHead{
			{HeadSHA: strings.Repeat("c", 40), PullRequests: []int{1589, 1590}},
			{HeadSHA: strings.Repeat("c", 40), PullRequests: []int{1589, 1590}},
		},
		caveatSelection(1589, strings.Repeat("c", 40)),
		caveatSelection(1590, strings.Repeat("c", 40)),
	)

	err := refusedSurveyPage(t, report)
	if !strings.Contains(err.Error(), "more than one group") {
		t.Fatalf("refusal %q does not say the commit was grouped twice", err)
	}
	if !strings.Contains(err.Error(), strings.Repeat("c", 40)) {
		t.Fatalf("refusal %q does not name the commit", err)
	}
}

// One commit written two ways is one commit, the rule the grouping itself
// keeps, so a second group under a different spelling is the same repetition
// and is refused the same way.
func TestACommitGroupedTwiceUnderTwoSpellingsIsRefused(t *testing.T) {
	lower := strings.Repeat("c", 40)
	report := sharedHeadSurveyOf(
		[]surveySharedHead{
			{HeadSHA: lower, PullRequests: []int{1589, 1590}},
			{HeadSHA: " " + strings.ToUpper(lower), PullRequests: []int{1589, 1590}},
		},
		caveatSelection(1589, lower),
		caveatSelection(1590, lower),
	)

	if err := refusedSurveyPage(t, report); !strings.Contains(err.Error(), "more than one group") {
		t.Fatalf("refusal %q does not say the commit was grouped twice", err)
	}
}

// Two clashes on one survey are two commits and are perfectly ordinary. The
// refusal is about one commit stated twice, never about a set with more than
// one clash in it.
func TestTwoCommitsEachGroupedOnceAreNotRefused(t *testing.T) {
	first := strings.Repeat("c", 40)
	second := strings.Repeat("d", 40)
	report := sharedHeadSurveyOf(
		[]surveySharedHead{
			{HeadSHA: first, PullRequests: []int{1589, 1590}},
			{HeadSHA: second, PullRequests: []int{1591, 1592}},
		},
		caveatSelection(1589, first),
		caveatSelection(1590, first),
		caveatSelection(1591, second),
		caveatSelection(1592, second),
	)

	page := renderCaveatSurvey(t, report)
	if !strings.Contains(page, "shared head "+first+": #1589, #1590") ||
		!strings.Contains(page, "shared head "+second+": #1591, #1592") {
		t.Fatalf("page %q does not state both clashes", page)
	}
}

// The section is now as long as the candidates allow and no longer: every
// group names at least two of them, every candidate is at one commit, and no
// commit is grouped twice.
func TestTheSharedHeadsAreBoundedByTheCandidates(t *testing.T) {
	candidates := make([]surveyedPullRequest, 0, 8)
	groups := make([]surveySharedHead, 0, 4)
	for pair := 0; pair < 4; pair++ {
		commit := strings.Repeat(string(rune('a'+pair)), 40)
		first, second := 1600+pair*2, 1601+pair*2
		candidates = append(candidates,
			caveatSelection(first, commit), caveatSelection(second, commit))
		groups = append(groups, surveySharedHead{
			HeadSHA: commit, PullRequests: []int{first, second},
		})
	}
	report := sharedHeadSurveyOf(groups, candidates...)

	if groupings := strings.Count(renderCaveatSurvey(t, report), "shared head "); groupings > len(candidates)/2 {
		t.Fatalf("%d groups over %d candidates", groupings, len(candidates))
	}
}
