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
	if err := checkNamedComparisons(report); err != nil {
		return err
	}
	if err := checkBlockingClaims(report); err != nil {
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

// checkNamedComparisons refuses a pairing this page cannot print a readable
// line for.
//
// renderComparison writes one line per pairing and that line is made of four
// fields: the catalog task, the conclusion this plane saw, the conclusion
// Actions reached, and the Actions job the caller said covers the task. Three
// of the four are load-bearing and nothing read them. A pairing with no task
// renders "  : ra8ci success, Actions success [build (ubuntu-latest)]", a line
// under a section heading that says which grade it got and never says what got
// it. A pairing with no observed conclusion renders "ra8ci , Actions failure",
// which reads as a plane that saw nothing rather than one whose answer went
// missing on the way to the page. A pairing with no Actions job renders a
// trailing "[]", and renderComparison's own comment is the argument against
// that one: a line without the job name cannot be checked by the person
// reading it, which is the only thing this page is for.
//
// The fourth field is deliberately not read here. An empty ActionsConclusion
// is a real state with a real meaning, the one ShadowObservation documents as
// "it has not completed", and the page already states it as "(not completed)"
// rather than leaving a hole in the sentence. It is also what puts the pairing
// in the indeterminate section, the reason the report holds the required
// check. Refusing it would refuse the most ordinary unclean report there is.
//
// The comparison's own HeadSHA is not read either. It is carried on every
// pairing and printed on none of them: the commit is stated once, at the top,
// off report.HeadSHA. Whether a pairing was graded against the commit the page
// is about is a real question and a different one from what this page says,
// and reading a field nobody sees is the rule this family of checks has kept
// to since the page checks began.
//
// The existing observation sentinel carries it, the same one the miscount and
// the empty report return. Nothing a real comparison writes is refused:
// ShadowObservation.validate already requires a valid task name, a known
// observed conclusion and a non-empty Actions job before CompareShadowRun will
// grade the pairing at all, so a report reaching this renderer with one of
// them missing was assembled somewhere that validation is not.
func checkNamedComparisons(report ShadowReport) error {
	for _, comparison := range report.Comparisons {
		if strings.TrimSpace(comparison.Task) == "" {
			return fmt.Errorf("%w: a pairing graded %s names no task",
				ErrShadowObservationInvalid, comparison.Verdict)
		}
		if strings.TrimSpace(comparison.Observed) == "" {
			return fmt.Errorf("%w: %q names no conclusion of ours",
				ErrShadowObservationInvalid, comparison.Task)
		}
		if strings.TrimSpace(comparison.ActionsJob) == "" {
			return fmt.Errorf("%w: %q was compared against no Actions job",
				ErrShadowObservationInvalid, comparison.Task)
		}
	}
	return nil
}

// checkBlockingClaims refuses a conflicting pairing whose own blocking flags
// do not say what its line says they say.
//
// A conflict is the only grade on this page that prints a claim about what
// each side would have done with the merge. renderComparison writes
// "(ra8ci would block, Actions would merge)" off ActionsBlocks alone, and it
// can write that because a conflict is by construction the disagreeing case:
// compare() grades a pairing conflicting only where PlaneBlocks and
// ActionsBlocks differ, so naming one side names the other. PlaneBlocks is
// carried on every pairing and read nowhere else in this package.
//
// That leaves the claim resting on an invariant nothing checked. A pairing
// graded conflicting whose two flags agree renders a sentence its own fields
// contradict, and it renders it in the section the page leads with, under a
// heading that says ra8ci and Actions disagree about the merge. The worst of
// the two readings is the one where both sides would block: the line tells an
// operator that ra8ci would block a merge Actions would have let through,
// which is the exact shape of the evidence that keeps a required check where
// it is, over a pairing where both planes agreed to block. Nobody reading it
// can tell, because the line is otherwise ordinary.
//
// Only the conflicting pairings are read. The flags reach the page nowhere
// else: an agreed or divergent line prints no blocking claim, and an
// indeterminate pairing has no Actions verdict to gate on at all. Holding
// those to their flags would refuse pages over a field the reader is never
// shown, which is the same line #1664 drew around HeadSHA.
//
// It is refused rather than reworded. The page cannot know which of the two
// fields is the wrong one, and a line that quietly drops the claim leaves a
// conflict section whose lines no longer say what the conflict was.
//
// The existing observation sentinel carries it, and nothing a real comparison
// writes is refused: compare() sets both flags from blockingConclusion and
// reaches the conflicting case only where they differ.
func checkBlockingClaims(report ShadowReport) error {
	for _, comparison := range report.Comparisons {
		if comparison.Verdict != ShadowConflicting || comparison.PlaneBlocks != comparison.ActionsBlocks {
			continue
		}
		would := "merge"
		if comparison.PlaneBlocks {
			would = "block"
		}
		return fmt.Errorf("%w: %q disagreed about the merge, and both sides would %s",
			ErrShadowObservationInvalid, comparison.Task, would)
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
