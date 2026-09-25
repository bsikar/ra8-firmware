// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// nextSurveyRunID hands out a run identifier no other standing in this file
// carries. A run is on one commit under one name, so two groups naming the
// same run is a document contradicting itself, and a fixture that does it by
// accident tests the refusal rather than the bound it was written for.
var nextSurveyRunID int64

func surveyRunID() int64 {
	nextSurveyRunID++
	return nextSurveyRunID
}

// standingSurveyOf builds a survey with the two grouping sections filled in,
// and with the listings those groups are derived from.
//
// The groupings are what the page renders, and a survey large enough to test
// a bound is one no real listing in this box would produce, so they are
// assembled here rather than surveyed. The listings are filled in from them
// because the page reads one against the other: a group naming a run the
// survey never accounted for is refused as a contradiction, which is a
// different answer from the one these tests are about.
//
// The tasks are settled. A conflicting one would print a line of its own and
// these tests count the page's lines.
func standingSurveyOf(contested []reconcileContestedStanding, unplanned []reconcileUnplannedStanding) reconcileReport {
	report := reconcileReport{
		Commit:            strings.Repeat("a", 40),
		Mode:              "authoritative",
		Settled:           true,
		ContestedStanding: contested,
		UnplannedStanding: unplanned,
	}
	for _, standing := range unplanned {
		for _, run := range standing.Runs {
			report.UnplannedRun = append(report.UnplannedRun, reconcileUnplannedRun{
				ID:         run,
				Name:       "other / build",
				Identifier: standing.Identifier,
			})
		}
	}
	report.Unplanned = len(report.UnplannedRun)
	for _, standing := range contested {
		for _, run := range standing.Runs {
			report.Tasks = append(report.Tasks, reconcileSurvey{
				Task:     run.Task,
				Name:     run.Name,
				Decision: decisionToken(github.PublishSettled),
				Published: []reconcileSurveyedRun{{
					ID:         run.ID,
					Identifier: standing.Identifier,
				}},
			})
		}
	}
	return report
}

// contestedStandingOf is one standing carrying the given number of runs under
// names tasks plan.
func contestedStandingOf(identifier string, runs int) reconcileContestedStanding {
	standing := reconcileContestedStanding{Identifier: identifier}
	for run := 0; run < runs; run++ {
		standing.Runs = append(standing.Runs, reconcileContestedRun{
			ID: surveyRunID(), Task: "build", Name: "ra8ci / build",
		})
	}
	return standing
}

// unplannedStandingOf is the same for the runs no task plans.
func unplannedStandingOf(identifier string, runs int) reconcileUnplannedStanding {
	standing := reconcileUnplannedStanding{Identifier: identifier}
	for run := 0; run < runs; run++ {
		standing.Runs = append(standing.Runs, surveyRunID())
	}
	return standing
}

// refusedReconcilePage renders a survey that must be refused and answers with
// the refusal. Nothing may be written above it: a caller handing the page
// straight to a terminal should not be left with half of one over the error.
func refusedReconcilePage(t *testing.T, report reconcileReport) error {
	t.Helper()
	page := &bytes.Buffer{}
	err := RenderReconcileSurvey(page, report)
	if !errors.Is(err, ErrReconcilePageTooLarge) {
		t.Fatalf("err = %v, want a refusal", err)
	}
	if page.Len() != 0 {
		t.Fatalf("page %q was written above the refusal", page)
	}
	return err
}

// The bound that existed guarded the one slice the page never prints. A
// survey with more unplanned runs than that bound, grouped into a standing
// or two, is a two-line page and has to render.
func TestASurveyOfManyUnplannedRunsIsStillItsTwoLines(t *testing.T) {
	report := standingSurveyOf(nil, []reconcileUnplannedStanding{
		unplannedStandingOf("a stranger", maxRenderedSurveyRuns),
		unplannedStandingOf("somebody else", maxRenderedSurveyRuns),
	})
	if report.Unplanned != maxRenderedSurveyRuns*2 {
		t.Fatalf("the survey lists %d runs, want %d", report.Unplanned, maxRenderedSurveyRuns*2)
	}

	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("render: %v", err)
	}
	if !strings.Contains(page.String(), "no task plans, a stranger:") {
		t.Fatalf("page %q does not state the standing", page)
	}
	if lines := strings.Count(page.String(), "\n"); lines != 4 {
		t.Fatalf("page has %d lines, want 4", lines)
	}
}

// One standing is one line, so the number of standings is what decides
// whether the page can be read at all.
func TestTooManyStandingsAreRefusedRatherThanCut(t *testing.T) {
	contested := make([]reconcileContestedStanding, 0, maxRenderedSurveyStandings+1)
	unplanned := make([]reconcileUnplannedStanding, 0, maxRenderedSurveyStandings+1)
	for standing := 0; standing <= maxRenderedSurveyStandings; standing++ {
		contested = append(contested, contestedStandingOf("a stranger", 1))
		unplanned = append(unplanned, unplannedStandingOf("a stranger", 1))
	}

	under := refusedReconcilePage(t, standingSurveyOf(contested, nil))
	if !strings.Contains(under.Error(), "under names we plan") {
		t.Fatalf("refusal %q does not name the grouping", under)
	}
	no := refusedReconcilePage(t, standingSurveyOf(nil, unplanned))
	if !strings.Contains(no.Error(), "no task plans") {
		t.Fatalf("refusal %q does not name the grouping", no)
	}
}

// The runs on a standing are named, never counted, so one standing can be a
// line as long as the whole page was ever meant to be.
func TestAStandingNamingTooManyRunsIsRefusedRatherThanCut(t *testing.T) {
	for _, one := range []struct {
		named  string
		report reconcileReport
	}{
		{"under names we plan", standingSurveyOf(
			[]reconcileContestedStanding{contestedStandingOf("a stranger", maxRenderedSurveyRuns+1)}, nil)},
		{"no task plans", standingSurveyOf(
			nil, []reconcileUnplannedStanding{unplannedStandingOf("a stranger", maxRenderedSurveyRuns+1)})},
	} {
		err := refusedReconcilePage(t, one.report)
		if !strings.Contains(err.Error(), one.named) {
			t.Fatalf("refusal %q does not name the grouping", err)
		}
		if !strings.Contains(err.Error(), "a stranger") {
			t.Fatalf("refusal %q does not name the standing to look at", err)
		}
	}
}

// A refusal that named a bare total would send a reader back to the document
// to work out which standing to look at, which is the walk the page exists
// to save.
func TestTheRefusalNamesTheStandingAndItsCount(t *testing.T) {
	err := refusedReconcilePage(t, standingSurveyOf(
		[]reconcileContestedStanding{contestedStandingOf("somebody else", maxRenderedSurveyRuns+1)}, nil))
	if !strings.Contains(err.Error(), "somebody else") ||
		!strings.Contains(err.Error(), "201 runs") {
		t.Fatalf("refusal %q does not say what was too large", err)
	}
}

// A survey sitting exactly on the bounds is a page, not a refusal. An
// off-by-one here refuses a survey that can be read.
func TestASurveyOnTheBoundsIsStillRendered(t *testing.T) {
	contested := make([]reconcileContestedStanding, 0, maxRenderedSurveyStandings)
	for standing := 0; standing < maxRenderedSurveyStandings; standing++ {
		// One group per standing is how the survey assembles these,
		// so the identifiers differ: two groups under one identifier
		// is a contradiction, not a bound.
		contested = append(contested, contestedStandingOf(fmt.Sprintf("a stranger %d", standing), 1))
	}
	report := standingSurveyOf(contested, []reconcileUnplannedStanding{
		unplannedStandingOf("a stranger", maxRenderedSurveyRuns),
	})

	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("render: %v", err)
	}
	if lines := strings.Count(page.String(), "\n"); lines != maxRenderedSurveyStandings+3 {
		t.Fatalf("page has %d lines, want %d", lines, maxRenderedSurveyStandings+3)
	}
}

// The bounds are read before the counts are checked, so an oversize survey
// is answered for its size rather than for whatever its counts say.
func TestTheBoundsAreReadBeforeTheCounts(t *testing.T) {
	report := standingSurveyOf(
		[]reconcileContestedStanding{contestedStandingOf("a stranger", maxRenderedSurveyRuns+1)}, nil)
	report.Conflict = 7

	err := refusedReconcilePage(t, report)
	if errors.Is(err, ErrReconcilePageInvalid) {
		t.Fatalf("err = %v, want the size refusal", err)
	}
}

// A survey this plane produced is never refused by its own page. The bounds
// are about a document too large to read, not about the ordinary answer.
func TestASurveysOwnStandingsAreNotRefused(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	settled := publishedAs(plan, 101, "completed", plan.Run.Conclusion, plan.Run.Title)

	if err := checkRenderedSurveyBounds(surveyOf(t, []plannedCheckRun{plan}, listing(settled))); err != nil {
		t.Fatalf("a surveyed commit was refused: %v", err)
	}
}
