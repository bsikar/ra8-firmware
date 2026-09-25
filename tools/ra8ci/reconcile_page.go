// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"strings"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// The reconcile survey is the read publish-check-run tells an operator to
// make, and it has only ever been a JSON document. The shadow comparison and
// the shadow evidence both have a page beside their document for the same
// reason this one needs one: the answer is assembled from counts, a per-task
// listing, and two groupings of runs by who posted them, and joining those by
// eye is the work the reader came to have done.
//
// Nothing here decides anything. The page states what the survey already
// decided, in the order the decisions matter, and every refusal below is
// about the page being unreadable rather than the survey being wrong.

var (
	// ErrReconcilePageTooLarge is returned rather than a cut page. A
	// survey past these bounds is a document to read with a machine, and
	// a page that quietly stops halfway is the one way this could report
	// a commit as clearer than it is.
	ErrReconcilePageTooLarge = errors.New("reconcile survey is too large to render")
	// ErrReconcilePageInvalid is returned for a survey that cannot be
	// stated: its own counts disagree with its tasks. The page is read to
	// decide whether a publish needs a person, so an assembled value that
	// contradicts itself is refused by name instead of printed.
	ErrReconcilePageInvalid = errors.New("reconcile survey does not describe itself")
)

const (
	// maxRenderedSurveyTasks bounds the per-task section.
	maxRenderedSurveyTasks = 500
	// maxRenderedSurveyRuns bounds each grouping of runs.
	maxRenderedSurveyRuns = 200
)

// RenderReconcileSurvey writes one commit's survey as the page it is read
// from.
//
// The verdict is first and states the decision rather than the counts, the
// convention the other two pages keep. The sections that follow are in
// decision order, never alphabetical: the tasks that conflict, then the runs
// under names we plan that this plane did not post, then the runs no task
// plans at all. That is the order the work is in, from the argument a person
// has to settle today to the leftover that can wait.
//
// A section with nothing in it is left out. "0 conflicting tasks" on every
// clean survey teaches a reader to skip the line that matters.
//
// Nothing is written on a refusal: a caller that hands the page straight to
// a terminal should not be left with half of one above the error.
func RenderReconcileSurvey(out io.Writer, report reconcileReport) error {
	if len(report.Tasks) > maxRenderedSurveyTasks {
		return fmt.Errorf("%w: %d tasks, %d at most",
			ErrReconcilePageTooLarge, len(report.Tasks), maxRenderedSurveyTasks)
	}
	if len(report.UnplannedRun) > maxRenderedSurveyRuns {
		return fmt.Errorf("%w: %d unplanned runs, %d at most",
			ErrReconcilePageTooLarge, len(report.UnplannedRun), maxRenderedSurveyRuns)
	}
	conflicting := conflictingSurveyTasks(report)
	if len(conflicting) != report.Conflict {
		return fmt.Errorf("%w: %d conflicting tasks counted, %d named",
			ErrReconcilePageInvalid, report.Conflict, len(conflicting))
	}
	page := &bytes.Buffer{}
	if report.Settled {
		fmt.Fprintf(page, "settled: every planned task is accounted for on %s (%s)\n",
			report.Commit, report.Mode)
	} else {
		fmt.Fprintf(page, "not settled: %s publish on %s\n", report.Mode, report.Commit)
	}
	fmt.Fprintf(page, "%d %s, %d to post, %d in flight, %d conflicting\n",
		len(report.Tasks), surveyPlural(len(report.Tasks), "task", "tasks"),
		report.Posting, report.Waiting, report.Conflict)
	for _, task := range conflicting {
		fmt.Fprintf(page, "conflicting: %s (%s)\n", task.Task, task.Name)
	}
	for _, standing := range report.ContestedStanding {
		named := make([]string, 0, len(standing.Runs))
		for _, run := range standing.Runs {
			named = append(named, fmt.Sprintf("#%d under %s (%s)", run.ID, run.Name, run.Task))
		}
		fmt.Fprintf(page, "under a name we plan, %s: %s\n",
			standing.Identifier, strings.Join(named, ", "))
	}
	for _, standing := range report.UnplannedStanding {
		named := make([]string, 0, len(standing.Runs))
		for _, run := range standing.Runs {
			named = append(named, fmt.Sprintf("#%d", run))
		}
		fmt.Fprintf(page, "no task plans, %s: %s\n",
			standing.Identifier, strings.Join(named, ", "))
	}
	_, err := out.Write(page.Bytes())
	return err
}

// conflictingSurveyTasks names the tasks the survey decided against, in the
// order the survey walked them. It reads the decision the survey already
// wrote rather than deciding again: two answers to one question in one
// command is how they drift apart.
func conflictingSurveyTasks(report reconcileReport) []reconcileSurvey {
	conflicting := make([]reconcileSurvey, 0, report.Conflict)
	for _, task := range report.Tasks {
		if task.Decision == decisionToken(github.PublishConflicts) {
			conflicting = append(conflicting, task)
		}
	}
	return conflicting
}

// surveyPlural picks the word for a count. A page a person reads says "1
// task", not "1 tasks": a line that reads as a template is one a reader
// stops believing was written about their commit.
func surveyPlural(count int, one, many string) string {
	if count == 1 {
		return one
	}
	return many
}
