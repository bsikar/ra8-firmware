// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"strings"
	"testing"
)

// namedShadowReport is one ordinary comparison of each grade, correctly
// counted, so a test can blank exactly one printed field on one pairing and
// nothing else on the page is wrong. It is countedShadowReport's fixture read
// for a different question, and the counts stay right because blanking a name
// moves no grade.
func namedShadowReport(t *testing.T) ShadowReport {
	t.Helper()
	return countedShadowReport(t)
}

// pairingIn returns the index of the fixture's pairing for a task, so a test
// names the line it breaks rather than an ordinal.
func pairingIn(t *testing.T, report ShadowReport, task string) int {
	t.Helper()
	for i, comparison := range report.Comparisons {
		if comparison.Task == task {
			return i
		}
	}
	t.Fatalf("the fixture has no pairing for %q", task)
	return -1
}

// *** THE READING THIS CHECK EXISTS FOR. A pairing with no task renders
// "  : ra8ci failure, Actions success [...]" under a section heading that
// says which grade it got and never says what got it. ***
func TestAPairingThatNamesNoTaskIsRefused(t *testing.T) {
	unnamed := namedShadowReport(t)
	unnamed.Comparisons[pairingIn(t, unnamed, "charlie")].Task = ""

	err := refusedShadowReport(t, unnamed)
	if !strings.Contains(err.Error(), "a pairing graded conflicting names no task") {
		t.Fatalf("err = %v, does not name the unnamed pairing by the grade it would print under", err)
	}
}

func TestAPairingNamedOnlyWithSpacesIsRefused(t *testing.T) {
	unnamed := namedShadowReport(t)
	unnamed.Comparisons[pairingIn(t, unnamed, "alpha")].Task = "   "

	err := refusedShadowReport(t, unnamed)
	if !strings.Contains(err.Error(), "names no task") {
		t.Fatalf("err = %v, does not refuse a task of spaces", err)
	}
}

func TestAPairingThatNamesNoConclusionOfOursIsRefused(t *testing.T) {
	silent := namedShadowReport(t)
	silent.Comparisons[pairingIn(t, silent, "bravo")].Observed = ""

	err := refusedShadowReport(t, silent)
	if !strings.Contains(err.Error(), `"bravo" names no conclusion of ours`) {
		t.Fatalf("err = %v, does not name the pairing whose own conclusion is missing", err)
	}
}

// *** renderComparison's own comment is the argument: a line without the job
// name cannot be checked by the person reading it, which is the only thing
// this page is for. ***
func TestAPairingComparedAgainstNoActionsJobIsRefused(t *testing.T) {
	unchecked := namedShadowReport(t)
	unchecked.Comparisons[pairingIn(t, unchecked, "alpha")].ActionsJob = ""

	err := refusedShadowReport(t, unchecked)
	if !strings.Contains(err.Error(), `"alpha" was compared against no Actions job`) {
		t.Fatalf("err = %v, does not name the pairing with no job to check it against", err)
	}
}

// *** THE DELIBERATE CONTRAST, DO NOT "COMPLETE" THIS CHECK BY REFUSING THE
// FOURTH FIELD. An empty ActionsConclusion is a real state with a real
// meaning, the one ShadowObservation documents as "it has not completed". The
// page states it rather than leaving a hole in the sentence, and it is what
// puts the pairing in the indeterminate section, the reason the report holds
// the required check. ***
func TestAPairingActionsHasNotFinishedStillRenders(t *testing.T) {
	var page strings.Builder
	if err := RenderShadowReport(&page, namedShadowReport(t)); err != nil {
		t.Fatalf("an ordinary unclean report was refused: %v", err)
	}
	if !strings.Contains(page.String(), "delta: ra8ci success, Actions (not completed)") {
		t.Fatalf("the unfinished pairing is not stated as unfinished:\n%s", page.String())
	}
}

func TestTheTaskIsReadBeforeTheJobItWasComparedAgainst(t *testing.T) {
	broken := namedShadowReport(t)
	at := pairingIn(t, broken, "alpha")
	broken.Comparisons[at].Task = ""
	broken.Comparisons[at].ActionsJob = ""

	err := refusedShadowReport(t, broken)
	if !strings.Contains(err.Error(), "names no task") {
		t.Fatalf("err = %v, does not refuse the pairing for its own name first", err)
	}
	if strings.Contains(err.Error(), "Actions job") {
		t.Fatalf("err = %v, names a pairing by a task it does not have", err)
	}
}

// The counts are the top of the page and the pairings are the lines under
// them, so a report wrong about both is refused for what a reader reads first.
func TestTheCountsAreReadBeforeTheNames(t *testing.T) {
	broken := namedShadowReport(t)
	broken.Comparisons[pairingIn(t, broken, "charlie")].Task = ""
	broken.Conflicting = 0

	err := refusedShadowReport(t, broken)
	if !strings.Contains(err.Error(), "0 conflicting counted, 1 graded so") {
		t.Fatalf("err = %v, does not refuse the miscount first", err)
	}
	if strings.Contains(err.Error(), "names no task") {
		t.Fatalf("err = %v, refuses a line under counts that are already wrong", err)
	}
}

func TestAnOrdinaryReportStillNamesEveryPairing(t *testing.T) {
	var page strings.Builder
	if err := RenderShadowReport(&page, namedShadowReport(t)); err != nil {
		t.Fatalf("an ordinary report was refused: %v", err)
	}
	for _, task := range []string{"alpha", "bravo", "charlie", "delta"} {
		if !strings.Contains(page.String(), "  "+task+": ra8ci ") {
			t.Fatalf("the page does not carry a line for %q:\n%s", task, page.String())
		}
	}
}
