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
	// maxRenderedSurveyStandings bounds each grouping section: one
	// standing is one line of the page.
	maxRenderedSurveyStandings = 200
	// maxRenderedSurveyRuns bounds the runs named on one standing's
	// line. The runs are named rather than counted, so a standing that
	// carries a thousand of them is a line a thousand runs wide.
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
	if err := checkRenderedSurveyBounds(report); err != nil {
		return err
	}
	if err := checkReconcileSurveyRuns(report); err != nil {
		return err
	}
	if err := checkReconcileSurveyCounts(report); err != nil {
		return err
	}
	if err := checkReconcileSurveyStandings(report); err != nil {
		return err
	}
	conflicting := conflictingSurveyTasks(report)
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

// checkRenderedSurveyBounds refuses a survey the page cannot state in full.
//
// The bounds are on what is WRITTEN, which is the part this one got wrong.
// The page never prints report.UnplannedRun: it prints the two groupings
// derived from it, one line per standing, with every run named on the line.
// So the one bound that existed guarded a slice nobody renders, while the
// sections that do get rendered had none. A survey carrying three hundred
// unplanned runs under two identifiers is a two-line page and was refused;
// a survey carrying three hundred standings, or one standing naming three
// hundred runs, is the page this bound was meant to stop and went through.
//
// Dropping the bound on the unplanned runs loses nothing. The document is
// already bounded at four megabytes where it is read, the count is already
// on the page, and the runs themselves are read from the document with a
// machine rather than from here.
//
// The refusals name which grouping, and which standing, rather than a bare
// total: the reader's next move is to look at that identifier.
func checkRenderedSurveyBounds(report reconcileReport) error {
	if len(report.Tasks) > maxRenderedSurveyTasks {
		return fmt.Errorf("%w: %d tasks, %d at most",
			ErrReconcilePageTooLarge, len(report.Tasks), maxRenderedSurveyTasks)
	}
	if len(report.ContestedStanding) > maxRenderedSurveyStandings {
		return fmt.Errorf("%w: %d standings under names we plan, %d at most",
			ErrReconcilePageTooLarge, len(report.ContestedStanding), maxRenderedSurveyStandings)
	}
	if len(report.UnplannedStanding) > maxRenderedSurveyStandings {
		return fmt.Errorf("%w: %d standings no task plans, %d at most",
			ErrReconcilePageTooLarge, len(report.UnplannedStanding), maxRenderedSurveyStandings)
	}
	for _, standing := range report.ContestedStanding {
		if len(standing.Runs) > maxRenderedSurveyRuns {
			return fmt.Errorf("%w: %s carries %d runs under names we plan, %d at most",
				ErrReconcilePageTooLarge, standing.Identifier, len(standing.Runs), maxRenderedSurveyRuns)
		}
	}
	for _, standing := range report.UnplannedStanding {
		if len(standing.Runs) > maxRenderedSurveyRuns {
			return fmt.Errorf("%w: %s carries %d runs no task plans, %d at most",
				ErrReconcilePageTooLarge, standing.Identifier, len(standing.Runs), maxRenderedSurveyRuns)
		}
	}
	return nil
}

// checkReconcileSurveyRuns refuses a survey that answers for one run more
// than once.
//
// The unplanned runs are the listing the standings are checked against, and
// nothing checked the listing itself. checkReconcileSurveyStandings reads it
// into a map keyed by run, so a run listed twice collapses into whichever
// answer came last: a group standing that run under the identifier the FIRST
// listing gave it is then refused as standing somewhere else, and the reader
// is sent after a run the page has just described wrongly. The count check
// does not catch it either, because it counts the listing's length and a
// repeat lengthens it.
//
// A run listed both as unplanned and under a task is the same finding from
// the other side. The two listings are meant to be disjoint: `UnplannedRuns`
// is given the planned names and keeps back everything under them, so a run
// in both is a document that contradicts itself about whether any task plans
// that run at all. Both of the page's grouping sections are derived from
// those listings, and the run would be read into both.
//
// Neither is a size bound. The listing is bounded where it is read and the
// page never prints it whole; a repeat is not a long page, it is a wrong one.
//
// The refusals say where the run stands, in the order the page states the
// groupings: the reader's next move is that run.
func checkReconcileSurveyRuns(report reconcileReport) error {
	listed := make(map[int64]struct{}, len(report.UnplannedRun))
	for _, run := range report.UnplannedRun {
		if _, twice := listed[run.ID]; twice {
			return fmt.Errorf("%w: run %d is listed more than once as one no task plans",
				ErrReconcilePageInvalid, run.ID)
		}
		listed[run.ID] = struct{}{}
	}
	for _, task := range report.Tasks {
		for _, run := range task.Published {
			if _, unplanned := listed[run.ID]; unplanned {
				return fmt.Errorf("%w: run %d is listed as one no task plans and under %s",
					ErrReconcilePageInvalid, run.ID, task.Name)
			}
		}
	}
	return nil
}

// checkReconcileSurveyCounts refuses a survey whose own counts and verdict
// disagree with its tasks.
//
// Only the conflicting count was ever checked, and it is the one count the
// reader can check for themselves: every conflicting task is named on a line
// below it. The two counts nobody can check were printed straight from the
// document, and the verdict was printed above all three. So the line read
// first could say a publish is settled while three conflicting lines follow
// it, and the line read second could say two tasks are waiting on a survey
// that decided nothing of the sort. That is the one reading this page must
// never produce: it is read to decide whether a publish needs a person.
//
// The counts are checked against the decisions the survey already wrote
// rather than decided again, the rule conflictingSurveyTasks keeps.
//
// The verdict is checked against the three counts and not against the tasks,
// because that is the survey's own rule for it: a publish is settled when
// nothing is to post, nothing is in flight and nothing conflicts.
//
// A settled survey carrying runs no task plans is NOT refused. The unplanned
// count deliberately moves neither the verdict nor the exit status, so a
// commit whose every planned task is accounted for is settled while an
// operator still has a leftover run to look at. That pair is ordinary and
// the page states both.
//
// Every refusal names what was counted and what was found, in that order:
// the reader's next move is to look at the listing, not at the total.
func checkReconcileSurveyCounts(report reconcileReport) error {
	counted := map[string]int{
		decisionToken(github.PublishNeeded):    report.Posting,
		decisionToken(github.PublishInFlight):  report.Waiting,
		decisionToken(github.PublishConflicts): report.Conflict,
	}
	named := map[string]int{}
	for _, task := range report.Tasks {
		named[task.Decision]++
	}
	// Stated in the order the page states them, so two wrong counts are
	// reported from the left rather than in map order.
	for _, decision := range []struct {
		token string
		as    string
	}{
		{decisionToken(github.PublishNeeded), "tasks to post"},
		{decisionToken(github.PublishInFlight), "tasks in flight"},
		{decisionToken(github.PublishConflicts), "conflicting tasks"},
	} {
		if counted[decision.token] != named[decision.token] {
			if decision.token == decisionToken(github.PublishConflicts) {
				return fmt.Errorf("%w: %d conflicting tasks counted, %d named",
					ErrReconcilePageInvalid, counted[decision.token], named[decision.token])
			}
			return fmt.Errorf("%w: %d %s counted, %d named",
				ErrReconcilePageInvalid, counted[decision.token], decision.as, named[decision.token])
		}
	}
	if report.Unplanned != len(report.UnplannedRun) {
		return fmt.Errorf("%w: %d runs no task plans counted, %d listed",
			ErrReconcilePageInvalid, report.Unplanned, len(report.UnplannedRun))
	}
	settled := report.Posting == 0 && report.Waiting == 0 && report.Conflict == 0
	if report.Settled != settled {
		return fmt.Errorf("%w: settled is %t over %d to post, %d in flight, %d conflicting",
			ErrReconcilePageInvalid, report.Settled, report.Posting, report.Waiting, report.Conflict)
	}
	return nil
}

// checkReconcileSurveyStandings refuses a grouping that is not about the runs
// the survey listed.
//
// The two groupings are the page's longest sections and the only lines that
// NAME runs. An operator reads "under a name we plan, foreign: #7 under ra8ci
// / build (build)" and goes and opens run 7, so a group naming a run this
// survey never accounted for, or standing it under an identifier the run does
// not carry, sends them to the wrong commit with the page's own authority
// behind it.
//
// Both groupings are derived where the survey is assembled, so nothing the
// surveying command writes is refused here. What this catches is a document
// from another build, or one assembled by hand, whose groupings and listings
// disagree.
//
// The refusals say where the run actually stands rather than that something
// is wrong: the reader's next move is the run, and the page has just told
// them where it is not.
func checkReconcileSurveyStandings(report reconcileReport) error {
	listed := make(map[int64]string, len(report.UnplannedRun))
	for _, run := range report.UnplannedRun {
		listed[run.ID] = run.Identifier
	}
	if err := checkSurveyGrouping(report.UnplannedStanding, "no task plans",
		func(standing reconcileUnplannedStanding) []int64 { return standing.Runs },
		func(standing reconcileUnplannedStanding) string { return standing.Identifier },
		func(run int64) (string, bool) {
			identifier, ok := listed[run]
			return identifier, ok
		}); err != nil {
		return err
	}
	ours := github.ExternalIDOurs.String()
	contested := make(map[int64]string, len(report.Tasks))
	for _, task := range report.Tasks {
		for _, run := range task.Published {
			if run.Identifier == ours {
				continue
			}
			contested[run.ID] = run.Identifier
		}
	}
	for _, standing := range report.ContestedStanding {
		// Ours is never a group. A run this plane posted under a name
		// it plans is the ordinary case, and the whole subject of this
		// section is the runs that are not that.
		if standing.Identifier == ours {
			return fmt.Errorf("%w: runs we posted are grouped under a name we plan",
				ErrReconcilePageInvalid)
		}
	}
	return checkSurveyGrouping(report.ContestedStanding, "under a name we plan",
		func(standing reconcileContestedStanding) []int64 {
			runs := make([]int64, 0, len(standing.Runs))
			for _, run := range standing.Runs {
				runs = append(runs, run.ID)
			}
			return runs
		},
		func(standing reconcileContestedStanding) string { return standing.Identifier },
		func(run int64) (string, bool) {
			identifier, ok := contested[run]
			return identifier, ok
		})
}

// checkSurveyGrouping reads one grouping against the listing it is derived
// from. The two groupings carry different run records and are checked the
// same way, so the walk is written once and handed what it needs: the runs
// on a standing, the standing's identifier, and where a run actually stands.
//
// An empty group is refused rather than skipped. Both groupings leave a
// standing out when no run carries it, so an empty one is a group about
// nothing, and it would print as an identifier with an empty list after it.
func checkSurveyGrouping[standing any](
	standings []standing,
	grouping string,
	runsOn func(standing) []int64,
	identifierOf func(standing) string,
	standsAs func(int64) (string, bool),
) error {
	groupedUnder := map[string]bool{}
	named := map[int64]string{}
	for _, group := range standings {
		identifier := identifierOf(group)
		runs := runsOn(group)
		if len(runs) == 0 {
			return fmt.Errorf("%w: %s %s groups no run",
				ErrReconcilePageInvalid, grouping, identifier)
		}
		if groupedUnder[identifier] {
			return fmt.Errorf("%w: %s %s is grouped more than once",
				ErrReconcilePageInvalid, grouping, identifier)
		}
		groupedUnder[identifier] = true
		for _, run := range runs {
			if under, already := named[run]; already {
				return fmt.Errorf("%w: %s run %d is grouped under %s and %s",
					ErrReconcilePageInvalid, grouping, run, under, identifier)
			}
			stands, listed := standsAs(run)
			if !listed {
				return fmt.Errorf("%w: %s run %d is grouped under %s and was not surveyed",
					ErrReconcilePageInvalid, grouping, run, identifier)
			}
			if stands != identifier {
				return fmt.Errorf("%w: %s run %d is grouped under %s and stands %s",
					ErrReconcilePageInvalid, grouping, run, identifier, stands)
			}
			named[run] = identifier
		}
	}
	return nil
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
