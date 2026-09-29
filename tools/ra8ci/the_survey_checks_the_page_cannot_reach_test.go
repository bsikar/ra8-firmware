// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// The reconcile page's survey checks are held by eleven test files that all
// drive them through the page. That leaves the arms the page itself cannot
// reach: a defensive branch the checks upstream already refuse, and the
// wording each refusal is written in. Those are worth holding directly,
// because the day somebody reorders the checks, the defensive branch is the
// only thing standing between a reader and a sentence with a gap in it.

// surveyGroup is a standing reduced to what checkSurveyGrouping actually
// asks for: an identifier and the runs grouped under it.
type surveyGroup struct {
	identifier string
	runs       []int64
}

// groupedRuns runs the shared grouping walk over surveyGroup values, with a
// listing the caller states, so the arms the two real groupings cannot both
// reach are reachable one at a time.
func groupedRuns(groups []surveyGroup, listing map[int64]string) error {
	return checkSurveyGrouping(groups, "a grouping",
		func(group surveyGroup) []int64 { return group.runs },
		func(group surveyGroup) string { return group.identifier },
		func(run int64) (string, bool) {
			stands, listed := listing[run]
			return stands, listed
		})
}

// A standing named twice is two groups about one thing, and the second is
// refused rather than merged into the first: merging would silently drop
// whichever list the page printed second.
func TestAStandingGroupedTwiceIsRefused(t *testing.T) {
	err := groupedRuns([]surveyGroup{
		{identifier: "queued", runs: []int64{7}},
		{identifier: "queued", runs: []int64{9}},
	}, map[int64]string{7: "queued", 9: "queued"})
	if !errors.Is(err, ErrReconcilePageInvalid) {
		t.Fatalf("err=%v; want the page refused", err)
	}
	if !strings.Contains(err.Error(), "grouped more than once") {
		t.Fatalf("err=%q; want it to name the repeated grouping", err)
	}
	if !strings.Contains(err.Error(), "queued") {
		t.Fatalf("err=%q; want it to name the standing", err)
	}
}

// The ordinary case, so the refusals above are about what they say they are.
func TestAnOrdinaryGroupingIsAccepted(t *testing.T) {
	if err := groupedRuns([]surveyGroup{
		{identifier: "queued", runs: []int64{7, 9}},
		{identifier: "in_progress", runs: []int64{11}},
	}, map[int64]string{7: "queued", 9: "queued", 11: "in_progress"}); err != nil {
		t.Fatalf("a sound grouping was refused: %v", err)
	}
}

// Two standings that each claim the same run: the second is refused, and the
// refusal names both standings, because the reader's question is which of the
// two the run actually stands under.
func TestARunGroupedUnderTwoStandingsNamesBoth(t *testing.T) {
	err := groupedRuns([]surveyGroup{
		{identifier: "queued", runs: []int64{7}},
		{identifier: "in_progress", runs: []int64{7}},
	}, map[int64]string{7: "queued"})
	if !errors.Is(err, ErrReconcilePageInvalid) {
		t.Fatalf("err=%v; want the page refused", err)
	}
	for _, named := range []string{"queued", "in_progress", "7"} {
		if !strings.Contains(err.Error(), named) {
			t.Errorf("err=%q; want it to name %s", err, named)
		}
	}
}

// checkContestedRunNames carries a defensive continue for a run the listing
// does not hold, because the grouping check upstream already refuses that and
// refuses it as what it is. Held here directly: if the checks are ever
// reordered, this is what keeps an unlisted run from being compared against a
// zero record and reported as a blank-name disagreement.
func TestAContestedRunTheListingDoesNotHoldIsLeftToTheGroupingCheck(t *testing.T) {
	standings := []reconcileContestedStanding{{
		Identifier: "queued",
		Runs:       []reconcileContestedRun{{ID: 7, Task: "build", Name: "ra8ci / Build"}},
	}}
	if err := checkContestedRunNames(standings, map[int64]reconcileContestedRun{}); err != nil {
		t.Fatalf("an unlisted run was judged on its name: %v", err)
	}
}

// The name and the task are two different answers, and each disagreement is
// reported as its own. A check run name is GitHub's own string, matched with
// no folding of case, which is the deliberate contrast with a commit.
func TestAContestedRunStatedUnderTheWrongNameOrTaskIsRefused(t *testing.T) {
	for name, stated := range map[string]reconcileContestedRun{
		"a different check run name":  {ID: 7, Task: "build", Name: "ra8ci / Compile"},
		"the same name in lower case": {ID: 7, Task: "build", Name: "ra8ci / build"},
		"a different task":            {ID: 7, Task: "compile", Name: "ra8ci / Build"},
		"no name at all":              {ID: 7, Task: "build", Name: ""},
		"no task at all":              {ID: 7, Task: "", Name: "ra8ci / Build"},
	} {
		t.Run(name, func(t *testing.T) {
			err := checkContestedRunNames(
				[]reconcileContestedStanding{{Identifier: "queued", Runs: []reconcileContestedRun{stated}}},
				map[int64]reconcileContestedRun{7: {ID: 7, Task: "build", Name: "ra8ci / Build"}})
			if !errors.Is(err, ErrReconcilePageInvalid) {
				t.Fatalf("err=%v; want the page refused", err)
			}
			if !strings.Contains(err.Error(), "run 7") {
				t.Fatalf("err=%q; want it to name the run", err)
			}
		})
	}
}

// A run stated exactly as the listing publishes it is accepted, which is what
// makes the case-sensitivity above a decision rather than an accident.
func TestAContestedRunStatedAsPublishedIsAccepted(t *testing.T) {
	published := reconcileContestedRun{ID: 7, Task: "build", Name: "ra8ci / Build"}
	if err := checkContestedRunNames(
		[]reconcileContestedStanding{{Identifier: "queued", Runs: []reconcileContestedRun{published}}},
		map[int64]reconcileContestedRun{7: published}); err != nil {
		t.Fatalf("a matching run was refused: %v", err)
	}
}

// Both refusals name what to go and look at, so neither can print as a
// sentence with a gap where the subject should be. A blank name is the case
// that produces the gap, and it is the one a survey of a half-published run
// actually carries.
func TestABlankNameIsStatedAsUnnamedRatherThanLeftAsAGap(t *testing.T) {
	for _, blank := range []string{"", " ", "\t", "\n", "   \t\n "} {
		if stated := statedSurveyCheckRun(blank); stated != "an unnamed check run" {
			t.Errorf("a check run named %q is stated as %q", blank, stated)
		}
		if stated := statedSurveyTask(blank); stated != "an unnamed task" {
			t.Errorf("a task named %q is stated as %q", blank, stated)
		}
	}
	if stated := statedSurveyCheckRun("  ra8ci / Build  "); stated != "the check run ra8ci / Build" {
		t.Errorf("a named check run is stated as %q", stated)
	}
	if stated := statedSurveyTask("  build  "); stated != "task build" {
		t.Errorf("a named task is stated as %q", stated)
	}
}

// An empty group is refused rather than skipped. Both real groupings leave a
// standing out when no run carries it, so an empty one is a group about
// nothing, and it would print as an identifier with an empty list under it.
func TestAGroupAboutNothingIsRefused(t *testing.T) {
	for name, groups := range map[string][]surveyGroup{
		"a standing with no runs": {{identifier: "queued"}},
		"an empty list of runs":   {{identifier: "queued", runs: []int64{}}},
		"one sound group and one not": {
			{identifier: "queued", runs: []int64{7}},
			{identifier: "in_progress"},
		},
	} {
		t.Run(name, func(t *testing.T) {
			err := groupedRuns(groups, map[int64]string{7: "queued"})
			if !errors.Is(err, ErrReconcilePageInvalid) {
				t.Fatalf("err=%v; want the page refused", err)
			}
			if !strings.Contains(err.Error(), "groups no run") {
				t.Fatalf("err=%q; want it to say the group is about nothing", err)
			}
		})
	}
}

// Ours is never a group. A run this plane posted under a name it plans is the
// ordinary case, and the whole subject of the contested section is the runs
// that are not that, so a survey that groups ours there is refused before the
// grouping is walked.
func TestASurveyThatGroupsOurOwnRunsAsContestedIsRefused(t *testing.T) {
	ours := github.ExternalIDOurs.String()
	report := reconcileReport{
		Commit: strings.Repeat("a", 40),
		ContestedStanding: []reconcileContestedStanding{{
			Identifier: ours,
			Runs:       []reconcileContestedRun{{ID: 7, Task: "build", Name: "ra8ci / Build"}},
		}},
	}
	err := checkReconcileSurveyStandings(report)
	if !errors.Is(err, ErrReconcilePageInvalid) {
		t.Fatalf("err=%v; want the page refused", err)
	}
	if !strings.Contains(err.Error(), "runs we posted") {
		t.Fatalf("err=%q; want it to say whose runs they are", err)
	}
}
