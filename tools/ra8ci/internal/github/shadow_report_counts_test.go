// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

// countedShadowReport is one ordinary comparison of each grade, counted correctly.
// Each test miscounts exactly one of them.
func countedShadowReport(t *testing.T) ShadowReport {
	t.Helper()
	report, err := CompareShadowRun([]ShadowObservation{
		observation("alpha", "success", "success"),
		observation("bravo", "failure", "timed_out"),
		observation("charlie", "failure", "success"),
		observation("delta", "success", ""),
	})
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	if report.Agreed != 1 || report.Divergent != 1 || report.Conflicting != 1 || report.Indeterminate != 1 {
		t.Fatalf("the fixture is not one of each grade: %+v", report)
	}
	return report
}

func refusedShadowReport(t *testing.T, report ShadowReport) error {
	t.Helper()
	var page strings.Builder
	err := RenderShadowReport(&page, report)
	if err == nil {
		t.Fatalf("the report rendered rather than being refused:\n%s", page.String())
	}
	if !errors.Is(err, ErrShadowObservationInvalid) {
		t.Fatalf("the refusal is not the page's own: %v", err)
	}
	if page.Len() != 0 {
		t.Fatalf("a refused render wrote to the stream: %q", page.String())
	}
	return err
}

// *** THE READING THIS CHECK EXISTS FOR. A report counting no conflicts over
// a pairing graded conflicting opens "every pairing was judged and none would
// have changed a merge outcome", because Clean() is those counters and
// nothing else, and then prints the conflicting section underneath it. ***
func TestAReportThatCountsNoConflictOverAConflictIsRefused(t *testing.T) {
	miscounted := countedShadowReport(t)
	miscounted.Conflicting = 0

	err := refusedShadowReport(t, miscounted)
	if !strings.Contains(err.Error(), "0 conflicting counted, 1 graded so") {
		t.Fatalf("err = %v, does not name the miscount", err)
	}
}

// The milder reading, and refused for the same reason: a hold line over a
// section that is not on the page sends an operator looking for a comparison
// nobody failed to make.
func TestAReportThatCountsAnUnjudgedPairingItDoesNotCarryIsRefused(t *testing.T) {
	miscounted := countedShadowReport(t)
	miscounted.Indeterminate = 2

	err := refusedShadowReport(t, miscounted)
	if !strings.Contains(err.Error(), "2 indeterminate counted, 1 graded so") {
		t.Fatalf("err = %v, does not name the miscount", err)
	}
}

// Every verdict is counted, not only the two that decide the hold. A
// divergent pairing moves no outcome, and the counts line still prints it
// beside the other three.
func TestADivergentMiscountIsRefusedEvenThoughItMovesNoOutcome(t *testing.T) {
	miscounted := countedShadowReport(t)
	miscounted.Divergent = 3

	err := refusedShadowReport(t, miscounted)
	if !strings.Contains(err.Error(), "3 divergent counted, 1 graded so") {
		t.Fatalf("err = %v, does not name the miscount", err)
	}
}

func TestAnAgreedMiscountIsRefused(t *testing.T) {
	miscounted := countedShadowReport(t)
	miscounted.Agreed = 0

	err := refusedShadowReport(t, miscounted)
	if !strings.Contains(err.Error(), "0 agreed counted, 1 graded so") {
		t.Fatalf("err = %v, does not name the miscount", err)
	}
}

// The sections are read in the order the page states them, so a report that
// miscounts two grades is refused for the one the decision turns on first.
func TestTheConflictsAreReadBeforeTheAgreements(t *testing.T) {
	miscounted := countedShadowReport(t)
	miscounted.Conflicting = 0
	miscounted.Agreed = 0

	err := refusedShadowReport(t, miscounted)
	if !strings.Contains(err.Error(), "conflicting") {
		t.Fatalf("err = %v, did not read the conflicts first", err)
	}
	if strings.Contains(err.Error(), "agreed") {
		t.Fatalf("err = %v, read the agreements before the conflicts", err)
	}
}

// A report is refused before it is bounded for size only where the size is
// fine: an oversized report is still refused as too large, the answer that
// already existed.
func TestAnOversizedReportIsStillRefusedForItsSize(t *testing.T) {
	report := countedShadowReport(t)
	for len(report.Comparisons) <= maxRenderedComparisons {
		report.Comparisons = append(report.Comparisons, report.Comparisons[0])
	}

	var page strings.Builder
	if err := RenderShadowReport(&page, report); !errors.Is(err, ErrShadowReportTooLarge) {
		t.Fatalf("an oversized report returned %v", err)
	}
}

// Nothing a real comparison writes is refused: CompareShadowRun increments
// exactly one counter per graded pairing, so its own report still renders
// with every section on it.
func TestAnHonestlyCountedReportStillRenders(t *testing.T) {
	var page strings.Builder
	if err := RenderShadowReport(&page, countedShadowReport(t)); err != nil {
		t.Fatalf("render: %v", err)
	}
	for _, want := range []string{
		"1 agreed, 1 divergent, 1 conflicting, 1 indeterminate\n",
		"conflicting (ra8ci and Actions disagree about the merge)\n",
		"indeterminate (Actions stated no outcome to compare)\n",
	} {
		if !strings.Contains(page.String(), want) {
			t.Fatalf("an honestly counted report does not carry %q:\n%s", want, page.String())
		}
	}
}
