// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

func renderedReport(t *testing.T, observations []ShadowObservation) string {
	t.Helper()
	report, err := CompareShadowRun(observations)
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	var page strings.Builder
	if err := RenderShadowReport(&page, report); err != nil {
		t.Fatalf("render: %v", err)
	}
	return page.String()
}

// The decision is read off the top of the page, so the verdict line states the
// decision and not the counts.
func TestACleanReportSaysSoBeforeAnythingElse(t *testing.T) {
	page := renderedReport(t, []ShadowObservation{
		observation("build-firmware", "success", "success"),
		observation("unit-tests", "failure", "timed_out"),
	})
	first := strings.SplitN(page, "\n", 3)
	if !strings.Contains(first[0], shadowHead) {
		t.Fatalf("first line does not name the commit: %q", first[0])
	}
	if !strings.Contains(first[1], "none would have changed a merge outcome") {
		t.Fatalf("clean verdict line is %q", first[1])
	}
	if strings.Contains(page, "holds the required check") {
		t.Fatal("a clean report must not say it holds the check")
	}
}

// Conflicts and pairings nobody judged are different jobs for the operator, so
// the hold line names which is present, and both when both are.
func TestTheHoldLineNamesWhatIsActuallyLeftToDo(t *testing.T) {
	conflicts := renderedReport(t, []ShadowObservation{observation("build-firmware", "failure", "success")})
	if !strings.Contains(conflicts, "holds the required check: 1 disagreed about the merge") ||
		strings.Contains(conflicts, "never judged") {
		t.Fatalf("conflict-only hold line wrong:\n%s", conflicts)
	}
	unjudged := renderedReport(t, []ShadowObservation{observation("build-firmware", "success", "stale")})
	if !strings.Contains(unjudged, "holds the required check: 1 were never judged") ||
		strings.Contains(unjudged, "disagreed") {
		t.Fatalf("indeterminate-only hold line wrong:\n%s", unjudged)
	}
	both := renderedReport(t, []ShadowObservation{
		observation("build-firmware", "failure", "success"),
		observation("unit-tests", "success", ""),
	})
	if !strings.Contains(both, "1 disagreed about the merge, 1 were never judged") {
		t.Fatalf("combined hold line wrong:\n%s", both)
	}
}

// The order on the page is the argument this file makes: what would have
// changed a merge first, then what still has to be compared, then the
// differences the gate cannot see, then the evidence for moving.
func TestSectionsAreOrderedByWhatTheDecisionTurnsOn(t *testing.T) {
	page := renderedReport(t, []ShadowObservation{
		observation("agreed-task", "success", "success"),
		observation("divergent-task", "failure", "timed_out"),
		observation("indeterminate-task", "success", "stale"),
		observation("conflicting-task", "failure", "success"),
	})
	positions := []int{
		strings.Index(page, "conflicting ("),
		strings.Index(page, "indeterminate ("),
		strings.Index(page, "divergent ("),
		strings.Index(page, "\nagreed\n"),
	}
	for i, at := range positions {
		if at < 0 {
			t.Fatalf("section %d missing from:\n%s", i, page)
		}
		if i > 0 && at < positions[i-1] {
			t.Fatalf("section %d is out of order:\n%s", i, page)
		}
	}
}

// A conflict line has to say which side would have blocked, because "they
// disagree" is not enough to act on.
func TestAConflictLineNamesWhichSideWouldHaveBlocked(t *testing.T) {
	page := renderedReport(t, []ShadowObservation{
		observation("plane-blocks", "failure", "success"),
		observation("actions-blocks", "success", "failure"),
	})
	if !strings.Contains(page, "(ra8ci would block, Actions would merge)") {
		t.Fatalf("plane-blocking conflict not explained:\n%s", page)
	}
	if !strings.Contains(page, "(Actions would block, ra8ci would merge)") {
		t.Fatalf("Actions-blocking conflict not explained:\n%s", page)
	}
	if strings.Count(page, "would block") != 2 {
		t.Fatalf("a non-conflict line carries a blocking claim:\n%s", page)
	}
}

// Every line carries the Actions job it was compared against, so the person
// reading the page can go and check it.
func TestEveryPairingLineCarriesTheJobItWasComparedAgainst(t *testing.T) {
	page := renderedReport(t, []ShadowObservation{
		observation("build-firmware", "success", "success"),
		observation("unit-tests", "failure", "success"),
	})
	for _, task := range []string{"build-firmware", "unit-tests"} {
		if !strings.Contains(page, task+" (ubuntu-latest)") {
			t.Fatalf("%s line has no Actions job:\n%s", task, page)
		}
	}
}

// An Actions job that has not completed reads as that, not as an empty field.
func TestAnUnfinishedJobIsNamedRatherThanLeftBlank(t *testing.T) {
	page := renderedReport(t, []ShadowObservation{observation("build-firmware", "success", "")})
	if !strings.Contains(page, "Actions (not completed)") {
		t.Fatalf("unfinished job rendered as:\n%s", page)
	}
}

// Grouping changes which lines sit together, never the task order inside a
// group, so two passes over one pull request stay diffable.
func TestTaskOrderSurvivesTheGrouping(t *testing.T) {
	page := renderedReport(t, []ShadowObservation{
		observation("zulu", "failure", "success"),
		observation("alpha", "failure", "success"),
		observation("mike", "failure", "success"),
	})
	if strings.Index(page, "alpha:") > strings.Index(page, "mike:") ||
		strings.Index(page, "mike:") > strings.Index(page, "zulu:") {
		t.Fatalf("task order lost:\n%s", page)
	}
}

func TestAReportThatCannotBeRenderedHonestlyIsRefused(t *testing.T) {
	if err := RenderShadowReport(nil, ShadowReport{}); err == nil {
		t.Fatal("a nil writer was accepted")
	}
	var page strings.Builder
	if err := RenderShadowReport(&page, ShadowReport{HeadSHA: shadowHead}); !errors.Is(err, ErrShadowObservationInvalid) {
		t.Fatalf("an empty report returned %v", err)
	}
	report, err := CompareShadowRun([]ShadowObservation{observation("build-firmware", "success", "success")})
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	headless := report
	headless.HeadSHA = "ra8ci/dev"
	if err := RenderShadowReport(&page, headless); !errors.Is(err, ErrShadowObservationInvalid) {
		t.Fatalf("a report with no commit returned %v", err)
	}
	oversized := report
	for len(oversized.Comparisons) <= maxRenderedComparisons {
		oversized.Comparisons = append(oversized.Comparisons, report.Comparisons[0])
	}
	if err := RenderShadowReport(&page, oversized); !errors.Is(err, ErrShadowReportTooLarge) {
		t.Fatalf("an oversized report returned %v", err)
	}
	if page.Len() != 0 {
		t.Fatalf("a refused render wrote to the stream: %q", page.String())
	}
}

// A write failure is reported, never swallowed: a report nobody received must
// not look like one that was read.
func TestAWriteFailureIsReported(t *testing.T) {
	report, err := CompareShadowRun([]ShadowObservation{observation("build-firmware", "success", "success")})
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	if err := RenderShadowReport(failingWriter{}, report); err == nil {
		t.Fatal("a failing writer was reported as a successful render")
	}
}

type failingWriter struct{}

func (failingWriter) Write([]byte) (int, error) { return 0, errors.New("closed") }
