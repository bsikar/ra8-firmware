// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// listedSurveyOf is a survey whose listings are given directly and whose
// groupings are given separately, which is the shape every other helper in
// these tests deliberately keeps consistent: standingSurveyOf derives the
// listing FROM the grouping, so it cannot state a run that nothing groups.
func listedSurveyOf(
	runs []reconcileUnplannedRun,
	unplanned []reconcileUnplannedStanding,
	contested []reconcileContestedStanding,
	tasks []reconcileSurvey,
) reconcileReport {
	report := reconcileReport{
		Commit:            strings.Repeat("a", 40),
		Mode:              "authoritative",
		Settled:           true,
		UnplannedRun:      runs,
		Unplanned:         len(runs),
		UnplannedStanding: unplanned,
		ContestedStanding: contested,
		Tasks:             tasks,
	}
	return report
}

// publishedTaskOf is one settled task publishing one run under the standing
// given. Both words the contested line prints are filled in: a blank one is
// refused before the grouping is ever read, which is a different answer.
func publishedTaskOf(run int64, identifier string) reconcileSurvey {
	return reconcileSurvey{
		Task:     "build",
		Name:     "ra8ci / build",
		Decision: decisionToken(github.PublishSettled),
		Published: []reconcileSurveyedRun{{
			ID:         run,
			Identifier: identifier,
		}},
	}
}

// The page never prints the listing. A run the document places under a name
// no task plans, and that no standing groups, is printed nowhere at all, so
// the page reads clean over a run that is really standing on the commit.
func TestAnUnplannedRunNoStandingGroupsIsRefused(t *testing.T) {
	report := listedSurveyOf(
		[]reconcileUnplannedRun{unplannedRunOf(7, "a stranger")},
		nil, nil, nil,
	)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 7") {
		t.Fatalf("the refusal does not name the run: %v", err)
	}
	if !strings.Contains(err.Error(), "no group names it") {
		t.Fatalf("the refusal does not say nothing groups it: %v", err)
	}
	if !strings.Contains(err.Error(), "a stranger") {
		t.Fatalf("the refusal does not say where the run stands: %v", err)
	}
}

// One run grouped and one not is the shape a survey assembled by hand
// actually takes, and the refusal names the one a reader would have looked
// for rather than the section.
func TestTheUngroupedUnplannedRunIsTheOneNamed(t *testing.T) {
	report := listedSurveyOf(
		[]reconcileUnplannedRun{
			unplannedRunOf(7, "a stranger"),
			unplannedRunOf(9, "a stranger"),
		},
		[]reconcileUnplannedStanding{unplannedGroupOf("a stranger", 7)},
		nil, nil,
	)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 9") {
		t.Fatalf("the refusal does not name the ungrouped run: %v", err)
	}
}

// A contested run makes its task conflict, so the conflicting line still
// appears, and it names the task. Which run to open is said on the grouping
// line alone.
func TestAContestedRunNoStandingGroupsIsRefused(t *testing.T) {
	report := listedSurveyOf(nil, nil, nil,
		[]reconcileSurvey{publishedTaskOf(11, "foreign")},
	)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 11") {
		t.Fatalf("the refusal does not name the run: %v", err)
	}
	if !strings.Contains(err.Error(), "under a name we plan") {
		t.Fatalf("the refusal does not say which grouping is silent: %v", err)
	}
	if !strings.Contains(err.Error(), "foreign") {
		t.Fatalf("the refusal does not say where the run stands: %v", err)
	}
}

// Runs of ours are skipped where the contested grouping is built, because a
// run this plane posted under a name it plans is the ordinary case. A page
// over nothing but our own runs has no contested section and is not refused
// for the lack of one.
func TestARunOfOursNeedsNoStanding(t *testing.T) {
	report := listedSurveyOf(nil, nil, nil,
		[]reconcileSurvey{publishedTaskOf(11, github.ExternalIDOurs.String())},
	)

	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("a survey of our own runs was refused: %v", err)
	}
	if strings.Contains(page.String(), "under a name we plan") {
		t.Fatalf("our own run was grouped as contested: %q", page)
	}
}

// The groupings are read against the listings first. A group that is itself
// malformed is refused as that, not as the listing it leaves unspoken for.
func TestAGroupIsReadBeforeTheRunsItLeavesOut(t *testing.T) {
	report := listedSurveyOf(
		[]reconcileUnplannedRun{
			unplannedRunOf(7, "a stranger"),
			unplannedRunOf(9, "a stranger"),
		},
		[]reconcileUnplannedStanding{unplannedGroupOf("a stranger", 7, 7)},
		nil, nil,
	)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "is grouped under") {
		t.Fatalf("the refusal is not the repeated grouping: %v", err)
	}
	if strings.Contains(err.Error(), "no group names it") {
		t.Fatalf("the refusal is the missing group rather than the malformed one: %v", err)
	}
}

// The listing that says no task plans the run is read before the one under
// names tasks do plan, the order the standings are already checked in.
func TestTheUnplannedListingIsReadBeforeTheContestedOne(t *testing.T) {
	report := listedSurveyOf(
		[]reconcileUnplannedRun{unplannedRunOf(7, "a stranger")},
		nil, nil,
		[]reconcileSurvey{publishedTaskOf(11, "foreign")},
	)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 7") {
		t.Fatalf("the refusal does not name the unplanned run: %v", err)
	}
}

// A run stated under no standing at all is named for what it is, rather
// than refused with a gap where the standing goes.
func TestARunUnderNoStatedStandingIsNamedAsThat(t *testing.T) {
	report := listedSurveyOf(
		[]reconcileUnplannedRun{unplannedRunOf(7, "  ")},
		nil, nil, nil,
	)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "under no stated standing") {
		t.Fatalf("the refusal leaves a gap where the standing goes: %v", err)
	}
}

// The survey the command actually writes groups every run it lists, on both
// sides, and is rendered.
func TestASurveyThatGroupsEveryRunIsRendered(t *testing.T) {
	report := listedSurveyOf(
		[]reconcileUnplannedRun{unplannedRunOf(7, "a stranger")},
		[]reconcileUnplannedStanding{unplannedGroupOf("a stranger", 7)},
		[]reconcileContestedStanding{contestedGroupOf("foreign", 11)},
		[]reconcileSurvey{publishedTaskOf(11, "foreign")},
	)

	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("a well formed survey was refused: %v", err)
	}
	if !strings.Contains(page.String(), "no task plans, a stranger: #7") {
		t.Fatalf("the unplanned grouping is not on the page: %q", page)
	}
	if !strings.Contains(page.String(), "under a name we plan, foreign: #11") {
		t.Fatalf("the contested grouping is not on the page: %q", page)
	}
}
