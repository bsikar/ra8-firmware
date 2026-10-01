// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"strings"
	"testing"
)

// blockingShadowReport is countedShadowReport read for a third question: one
// ordinary comparison of each grade, correctly counted and fully named, so a
// test can break exactly one pairing's blocking flags and nothing else on the
// page is wrong. "charlie" is the conflicting one (ra8ci failure against an
// Actions success), which is the only line that prints a blocking claim.
func blockingShadowReport(t *testing.T) ShadowReport {
	t.Helper()
	report := countedShadowReport(t)
	at := pairingIn(t, report, "charlie")
	if report.Comparisons[at].Verdict != ShadowConflicting {
		t.Fatalf("the fixture's conflict is not graded conflicting: %+v", report.Comparisons[at])
	}
	if report.Comparisons[at].PlaneBlocks == report.Comparisons[at].ActionsBlocks {
		t.Fatalf("the fixture's conflict does not disagree about blocking: %+v", report.Comparisons[at])
	}
	return report
}

// *** THE READING THIS CHECK EXISTS FOR, AND THE WORSE OF THE TWO. Both sides
// would have blocked, and the line in the section the page leads with says
// ra8ci would block a merge Actions would have let through. That sentence is
// the exact shape of the evidence that keeps a required check where it is. ***
func TestAConflictBothSidesWouldHaveBlockedIsRefused(t *testing.T) {
	agreeing := blockingShadowReport(t)
	at := pairingIn(t, agreeing, "charlie")
	agreeing.Comparisons[at].PlaneBlocks = true
	agreeing.Comparisons[at].ActionsBlocks = true

	err := refusedShadowReport(t, agreeing)
	if !strings.Contains(err.Error(), `"charlie" disagreed about the merge, and both sides would block`) {
		t.Fatalf("err = %v, does not name the pairing and what both sides would have done", err)
	}
}

// The other reading, refused for the same reason: a conflict section whose
// line says one side would have blocked, over a pairing where neither would.
func TestAConflictNeitherSideWouldHaveBlockedIsRefused(t *testing.T) {
	agreeing := blockingShadowReport(t)
	at := pairingIn(t, agreeing, "charlie")
	agreeing.Comparisons[at].PlaneBlocks = false
	agreeing.Comparisons[at].ActionsBlocks = false

	err := refusedShadowReport(t, agreeing)
	if !strings.Contains(err.Error(), `"charlie" disagreed about the merge, and both sides would merge`) {
		t.Fatalf("err = %v, does not say both sides would have merged", err)
	}
}

// *** THE DELIBERATE GAP, DO NOT "COMPLETE" THIS CHECK BY HOLDING EVERY
// PAIRING TO ITS FLAGS. The flags reach this page on the conflicting lines
// and nowhere else: an agreed or divergent line prints no blocking claim, and
// an indeterminate pairing has no Actions verdict to gate on. Refusing a page
// over a field the reader is never shown is the line #1664 drew around
// HeadSHA, drawn again here. ***
func TestFlagsOnAPairingThatPrintsNoBlockingClaimStillRender(t *testing.T) {
	odd := blockingShadowReport(t)
	odd.Comparisons[pairingIn(t, odd, "alpha")].ActionsBlocks = true
	odd.Comparisons[pairingIn(t, odd, "delta")].PlaneBlocks = true

	var page strings.Builder
	if err := RenderShadowReport(&page, odd); err != nil {
		t.Fatalf("a page was refused over flags no line prints: %v", err)
	}
	if strings.Contains(page.String(), "alpha: ra8ci success, Actions success [alpha] (") {
		t.Fatalf("an agreed line grew a blocking claim:\n%s", page.String())
	}
}

// The claim is only as good as the names around it, so a pairing missing both
// is refused for the name a reader would have read it by.
func TestTheNamesAreReadBeforeTheBlockingClaim(t *testing.T) {
	broken := blockingShadowReport(t)
	at := pairingIn(t, broken, "charlie")
	broken.Comparisons[at].Task = ""
	broken.Comparisons[at].PlaneBlocks = broken.Comparisons[at].ActionsBlocks

	err := refusedShadowReport(t, broken)
	if !strings.Contains(err.Error(), "names no task") {
		t.Fatalf("err = %v, does not refuse the pairing for its own name first", err)
	}
	if strings.Contains(err.Error(), "both sides would") {
		t.Fatalf("err = %v, states a blocking claim for a pairing it cannot name", err)
	}
}

// The counts are the top of the page and this is a line under them, so a
// report wrong about both is refused for what a reader reads first.
func TestTheCountsAreReadBeforeTheBlockingClaim(t *testing.T) {
	broken := blockingShadowReport(t)
	at := pairingIn(t, broken, "charlie")
	broken.Comparisons[at].ActionsBlocks = broken.Comparisons[at].PlaneBlocks
	broken.Conflicting = 0

	err := refusedShadowReport(t, broken)
	if !strings.Contains(err.Error(), "0 conflicting counted, 1 graded so") {
		t.Fatalf("err = %v, does not refuse the miscount first", err)
	}
	if strings.Contains(err.Error(), "both sides would") {
		t.Fatalf("err = %v, refuses a line under counts that are already wrong", err)
	}
}

func TestAnOrdinaryConflictStillStatesWhichSideWouldBlock(t *testing.T) {
	var page strings.Builder
	if err := RenderShadowReport(&page, blockingShadowReport(t)); err != nil {
		t.Fatalf("an ordinary unclean report was refused: %v", err)
	}
	if !strings.Contains(page.String(), "(ra8ci would block, Actions would merge)") {
		t.Fatalf("the conflicting line no longer says which side would block:\n%s", page.String())
	}
}

// Nothing a real comparison writes is refused: compare() sets both flags from
// blockingConclusion and reaches the conflicting case only where they differ,
// so a report graded from its own observations always renders.
func TestAReportGradedFromItsOwnObservationsAlwaysRenders(t *testing.T) {
	report, err := CompareShadowRun([]ShadowObservation{
		observation("alpha", "failure", "success"),
		observation("bravo", "success", "failure"),
		observation("charlie", "cancelled", "success"),
		observation("delta", "success", "neutral"),
	})
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	if report.Conflicting < 1 {
		t.Fatalf("the fixture grades no conflict: %+v", report)
	}
	var page strings.Builder
	if err := RenderShadowReport(&page, report); err != nil {
		t.Fatalf("a report graded from its own observations was refused: %v", err)
	}
}
