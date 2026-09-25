// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"strings"
	"testing"
)

// unplannedGroupOf is one standing carrying exactly the runs it is given,
// rather than the generated ones the bounds tests use: these tests are about
// the numbers themselves.
func unplannedGroupOf(identifier string, runs ...int64) reconcileUnplannedStanding {
	return reconcileUnplannedStanding{Identifier: identifier, Runs: runs}
}

// contestedGroupOf is the same for the runs under names tasks plan. Both
// fields the line names are filled in: a blank one is refused before the
// number is ever read, which is a different answer from the one under test.
func contestedGroupOf(identifier string, runs ...int64) reconcileContestedStanding {
	standing := reconcileContestedStanding{Identifier: identifier}
	for _, run := range runs {
		standing.Runs = append(standing.Runs, reconcileContestedRun{
			ID: run, Task: "build", Name: "ra8ci / build",
		})
	}
	return standing
}

// The number is the whole of what the grouping hands a reader. A run
// numbered zero prints as "#0", which is a run to go and open that nobody
// can open.
func TestAnUnplannedRunGroupedAtNoNumberIsRefused(t *testing.T) {
	report := standingSurveyOf(nil, []reconcileUnplannedStanding{
		unplannedGroupOf("a stranger", 7, 0),
	})

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "is numbered 0") {
		t.Fatalf("the refusal does not say the run is unnumbered: %v", err)
	}
	if !strings.Contains(err.Error(), "a stranger") {
		t.Fatalf("the refusal does not say where the run stands: %v", err)
	}
}

// The other grouping prints its runs the same way and is read the same way.
func TestAContestedRunGroupedAtNoNumberIsRefused(t *testing.T) {
	report := standingSurveyOf([]reconcileContestedStanding{
		contestedGroupOf("foreign", 0),
	}, nil)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "is numbered 0") {
		t.Fatalf("the refusal does not say the run is unnumbered: %v", err)
	}
	if !strings.Contains(err.Error(), "foreign") {
		t.Fatalf("the refusal does not say where the run stands: %v", err)
	}
}

// A negative number is not a run either, and it would print as "#-3".
func TestANegativelyNumberedGroupedRunIsRefused(t *testing.T) {
	report := standingSurveyOf(nil, []reconcileUnplannedStanding{
		unplannedGroupOf("a stranger", -3),
	})

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "is numbered -3") {
		t.Fatalf("the refusal does not name the number: %v", err)
	}
}

// The number is read before where the run stands: every other answer about
// a run is keyed by its number, so a survey that disagrees with itself about
// an unnumbered run is refused as the unnumbered one.
func TestTheNumberIsReadBeforeTheStanding(t *testing.T) {
	report := standingSurveyOf(nil, []reconcileUnplannedStanding{
		unplannedGroupOf("a stranger", 0),
	})
	for run := range report.UnplannedRun {
		report.UnplannedRun[run].Identifier = "somebody else"
	}

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "is numbered 0") {
		t.Fatalf("the refusal does not lead with the number: %v", err)
	}
	if strings.Contains(err.Error(), "stands") {
		t.Fatalf("the refusal answers for the standing as well: %v", err)
	}
}

// Two unnumbered runs are refused as a repeated listing rather than as
// unnumbered ones, and that order is deliberate: the listings are read
// before the groupings they are checked against, and a run listed twice
// collapses into whichever answer came last whatever it is numbered.
func TestTwoUnnumberedRunsAreRefusedAsARepeatedListing(t *testing.T) {
	report := standingSurveyOf(nil, []reconcileUnplannedStanding{
		unplannedGroupOf("a stranger", 0, 0),
	})

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "is listed more than once") {
		t.Fatalf("the refusal does not lead with the repeated listing: %v", err)
	}
}

// Nothing an ordinary grouping carries is refused, and both sections still
// name their runs.
func TestOrdinaryGroupedRunsAreStated(t *testing.T) {
	report := standingSurveyOf([]reconcileContestedStanding{
		contestedGroupOf("foreign", 11, 12),
	}, []reconcileUnplannedStanding{
		unplannedGroupOf("a stranger", 7, 9),
	})

	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("render: %v", err)
	}
	if !strings.Contains(page.String(), "no task plans, a stranger: #7, #9") {
		t.Fatalf("page %q does not name the unplanned runs", page)
	}
	if !strings.Contains(page.String(), "under a name we plan, foreign: #11 under ra8ci / build (build)") {
		t.Fatalf("page %q does not name the contested runs", page)
	}
}
