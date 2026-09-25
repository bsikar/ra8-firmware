// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"io"
	"strings"
)

// CompareShadowRun grades a commit's pairings; this renders the grading for the
// operator who has to decide whether a required check may move. The decision is
// made by reading, so the order on the page is the order the decision is made
// in: the pairings that would have changed a merge outcome first, then the ones
// nobody judged, then the differences the gate cannot see, then the agreements.
// A report sorted by task name reads as a list of facts; this one reads as an
// answer.

// maxRenderedComparisons bounds a rendered report. A pull request has tens of
// checks, not thousands, and a report long enough to scroll past is one nobody
// reads to the end of.
const maxRenderedComparisons = 500

// ErrShadowReportTooLarge is returned for a report with more pairings than a
// person would read. It is a refusal rather than a truncation: a report cut
// short in the middle of its conflicts is worse than no report.
var ErrShadowReportTooLarge = errors.New("shadow report has too many comparisons to render")

// renderOrder is the order the sections are written in, and it is the argument
// this file makes: a conflict is why the required check stays where it is, an
// indeterminate pairing is a comparison that still has to be made, a divergence
// is worth an eye but changes no outcome, and an agreement is the evidence for
// moving.
var renderOrder = []ShadowVerdict{ShadowConflicting, ShadowIndeterminate, ShadowDivergent, ShadowAgreed}

// sectionHeadings names each section in the words of the decision rather than
// the name of the grade.
var sectionHeadings = map[ShadowVerdict]string{
	ShadowConflicting:   "conflicting (ra8ci and Actions disagree about the merge)",
	ShadowIndeterminate: "indeterminate (Actions stated no outcome to compare)",
	ShadowDivergent:     "divergent (different words, same effect on the gate)",
	ShadowAgreed:        "agreed",
}

// RenderShadowReport writes one commit's comparison.
//
// The verdict line is the first thing on the page and it states the decision,
// not the counts: a report is either a reason to hold the required check or it
// is not. A caller that wants the numbers has the ShadowReport itself.
func RenderShadowReport(out io.Writer, report ShadowReport) error {
	if out == nil {
		return errors.New("no writer for shadow report")
	}
	if !validCommitSHA(report.HeadSHA) || len(report.Comparisons) == 0 {
		return fmt.Errorf("%w: nothing to render", ErrShadowObservationInvalid)
	}
	if len(report.Comparisons) > maxRenderedComparisons {
		return fmt.Errorf("%w: %d", ErrShadowReportTooLarge, len(report.Comparisons))
	}
	if err := checkGradedComparisons(report); err != nil {
		return err
	}
	var page strings.Builder
	fmt.Fprintf(&page, "shadow comparison for %s\n", report.HeadSHA)
	if report.Clean() {
		page.WriteString("every pairing was judged and none would have changed a merge outcome\n")
	} else {
		page.WriteString("holds the required check: ")
		page.WriteString(holdReason(report))
		page.WriteString("\n")
	}
	fmt.Fprintf(&page, "%d agreed, %d divergent, %d conflicting, %d indeterminate\n",
		report.Agreed, report.Divergent, report.Conflicting, report.Indeterminate)
	for _, verdict := range renderOrder {
		section := comparisonsWith(report.Comparisons, verdict)
		if len(section) == 0 {
			continue
		}
		fmt.Fprintf(&page, "\n%s\n", sectionHeadings[verdict])
		for _, comparison := range section {
			page.WriteString(renderComparison(comparison))
		}
	}
	if _, err := io.WriteString(out, page.String()); err != nil {
		return fmt.Errorf("write shadow report: %w", err)
	}
	return nil
}

// checkGradedComparisons refuses a report whose counts are not about its own
// pairings.
//
// Everything a reader acts on at the top of this page comes off the four
// counters, and everything below them comes off the pairings. The counts
// line is printed from the counters. The verdict line above it is printed
// from Clean(), which is those same counters and nothing else. The sections
// under it are built by walking Comparisons and grading each line. Nothing
// read the two against each other.
//
// The worst reading is the one at the top. A report counting no conflicts
// over a pairing graded conflicting opens "every pairing was judged and none
// would have changed a merge outcome", then prints the conflicting section
// underneath it, and an operator who read the line the page leads with has
// been told the required check may move by the same page that shows why it
// may not. The milder reading is a hold over a section that is not there:
// "holds the required check: 1 were never judged" above a page with nothing
// indeterminate on it, which sends someone looking for a comparison nobody
// failed to make.
//
// Every verdict is counted, not only the two that decide the hold. A
// divergent pairing moves no outcome, but the counts line prints it beside
// the other three and a reader comparing two commits reads those four
// numbers as the shape of the run.
//
// It is refused rather than recounted here. The counters are written by
// CompareShadowRun beside the grading, in one walk, and a page that quietly
// corrects them is a page that disagrees with the ShadowReport its caller
// holds. A report that contradicts itself is not a report to render more
// carefully; it was assembled somewhere this command did not.
//
// The existing observation sentinel carries it, the one an empty report and
// a commitless report already return. It is not a size bound: the pairings
// are already bounded, and a miscount is not a long report, it is a wrong
// one.
//
// Nothing a real comparison writes is refused: CompareShadowRun increments
// exactly one counter per graded pairing.
func checkGradedComparisons(report ShadowReport) error {
	counted := map[ShadowVerdict]int{
		ShadowAgreed:        report.Agreed,
		ShadowDivergent:     report.Divergent,
		ShadowConflicting:   report.Conflicting,
		ShadowIndeterminate: report.Indeterminate,
	}
	for _, verdict := range renderOrder {
		graded := len(comparisonsWith(report.Comparisons, verdict))
		if counted[verdict] != graded {
			return fmt.Errorf("%w: %d %s counted, %d graded so",
				ErrShadowObservationInvalid, counted[verdict], verdict, graded)
		}
	}
	return nil
}

// holdReason says which of the two unclean conditions is present, both when
// both are. An operator reading "holds the required check" has to be told
// whether the work left is a disagreement to explain or a comparison still to
// make, because those are different jobs.
func holdReason(report ShadowReport) string {
	switch {
	case report.Conflicting > 0 && report.Indeterminate > 0:
		return fmt.Sprintf("%d disagreed about the merge, %d were never judged",
			report.Conflicting, report.Indeterminate)
	case report.Conflicting > 0:
		return fmt.Sprintf("%d disagreed about the merge", report.Conflicting)
	default:
		return fmt.Sprintf("%d were never judged", report.Indeterminate)
	}
}

// renderComparison is one line per pairing, carrying the Actions job it was
// compared against. A line without the job name cannot be checked by the person
// reading it, which is the only thing this page is for.
func renderComparison(comparison ShadowComparison) string {
	actions := comparison.ActionsConclusion
	if actions == "" {
		actions = "(not completed)"
	}
	line := fmt.Sprintf("  %s: ra8ci %s, Actions %s [%s]",
		comparison.Task, comparison.Observed, actions, comparison.ActionsJob)
	if comparison.Verdict == ShadowConflicting {
		blocked, merged := "ra8ci", "Actions"
		if comparison.ActionsBlocks {
			blocked, merged = "Actions", "ra8ci"
		}
		line += fmt.Sprintf(" (%s would block, %s would merge)", blocked, merged)
	}
	return line + "\n"
}

// comparisonsWith keeps the task ordering CompareShadowRun established, so the
// grouping changes which lines are together and never which order they are in.
func comparisonsWith(comparisons []ShadowComparison, verdict ShadowVerdict) []ShadowComparison {
	section := make([]ShadowComparison, 0, len(comparisons))
	for _, comparison := range comparisons {
		if comparison.Verdict == verdict {
			section = append(section, comparison)
		}
	}
	return section
}
