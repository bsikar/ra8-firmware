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
	if err := checkSurveySubject(report); err != nil {
		return err
	}
	if err := checkReconcileSurveyRuns(report); err != nil {
		return err
	}
	if err := checkReconcileSurveyDecisions(report); err != nil {
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

// checkSurveySubject refuses a survey that does not say what it is about.
//
// Every other check on this page reads the survey against itself: the counts
// against the tasks, the standings against the listings, the listings against
// each other. None of them reads the words the page actually prints, and four
// of those are carried straight out of the document: the commit and the mode
// on the line read first, and the task and the name on every conflicting
// line. A survey with a blank commit renders as "settled: every planned task
// is accounted for on  ()", which is a clean verdict about nothing, and a
// conflicting task with no name renders as "conflicting: build ()" over the
// one line an operator is meant to act on.
//
// Only the tasks the page NAMES are read, which is the rule the bounds
// settled: a settled task is never printed, so refusing a whole page over a
// line nobody reads takes a readable page away from an operator over a field
// they would never have seen. The same rule puts the standings here: an
// identifier is the whole of what groups a standing's runs, and a blank one
// prints "no task plans, : #7, #9".
//
// It is the same rule that reads a contested run's name and task and leaves
// an unplanned run's alone. A contested line names both, "#7 under ra8ci /
// build (build)", and the name is the point of the section: a stranger's run
// under a name we plan is a name branch protection may one day require, and
// "#7 under  (build)" is that finding with the answer missing. An unplanned
// standing prints its runs as bare numbers, so the name in that listing is
// a field nobody would have read here.
//
// Whitespace is not a statement. A commit of three spaces is a blank commit
// wearing a value, and it would print as one.
//
// Nothing here decides anything, and nothing a real survey writes is refused:
// the commit and the mode come from the arguments the command was given, and
// the task and the name come from the catalog.
func checkSurveySubject(report reconcileReport) error {
	if strings.TrimSpace(report.Commit) == "" {
		return fmt.Errorf("%w: the survey names no commit", ErrReconcilePageInvalid)
	}
	if strings.TrimSpace(report.Mode) == "" {
		return fmt.Errorf("%w: the survey on %s names no mode",
			ErrReconcilePageInvalid, report.Commit)
	}
	for _, task := range conflictingSurveyTasks(report) {
		if strings.TrimSpace(task.Task) == "" {
			return fmt.Errorf("%w: a conflicting task is not named",
				ErrReconcilePageInvalid)
		}
		if strings.TrimSpace(task.Name) == "" {
			return fmt.Errorf("%w: conflicting task %s names no check run",
				ErrReconcilePageInvalid, task.Task)
		}
	}
	for _, standing := range report.ContestedStanding {
		if strings.TrimSpace(standing.Identifier) == "" {
			return fmt.Errorf("%w: under a name we plan, a group of %d runs carries no standing",
				ErrReconcilePageInvalid, len(standing.Runs))
		}
		for _, run := range standing.Runs {
			// The check run is read first because it is what the
			// section is for: the task beside it says which of our
			// plans wanted that name, and a reader who has the name
			// can already go and look.
			if strings.TrimSpace(run.Name) == "" {
				return fmt.Errorf("%w: run %d grouped under %s names no check run",
					ErrReconcilePageInvalid, run.ID, standing.Identifier)
			}
			if strings.TrimSpace(run.Task) == "" {
				return fmt.Errorf("%w: run %d grouped under %s names no task",
					ErrReconcilePageInvalid, run.ID, standing.Identifier)
			}
		}
	}
	for _, standing := range report.UnplannedStanding {
		if strings.TrimSpace(standing.Identifier) == "" {
			return fmt.Errorf("%w: no task plans, a group of %d runs carries no standing",
				ErrReconcilePageInvalid, len(standing.Runs))
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

// checkReconcileSurveyDecisions refuses a survey carrying a task whose
// decision this build cannot state.
//
// Every count on this page is bucketed by that one string. The counts are
// read against it, conflictingSurveyTasks selects the lines below them with
// it, and the verdict is read against the counts. Nothing read the string
// itself, and a decision outside the four the survey writes falls into none
// of the three buckets: the counts still agree with the tasks, because both
// sides of that comparison skip it, and the verdict still comes out settled,
// because the three counts are zero. The page then opens "settled: every
// planned task is accounted for" over a task it has not accounted for
// anywhere, and never prints a line about it. That is the one reading this
// page must never produce, and it is the reading a miscount would have been
// refused for.
//
// Every task is read, not only the ones the page names. This is the
// deliberate contrast with the subject check, which reads only the
// conflicting tasks because a settled task's name is never printed: a
// decision is not printed for any task at all, and it is still what the
// total on the second line and the verdict on the first are claims about.
// A task nobody can see is exactly the one that must not be silently
// dropped.
//
// A decision is matched EXACTLY, without trimming, and that is not the
// whitespace rule the subject checks keep. The counts are keyed by the raw
// string, so " conflicts" is counted under nothing whatever it looks like;
// trimming here would state a decision as well formed and leave the count it
// defeats to the check below, which cannot see it either.
//
// The blank decision is named separately because it is the one a reader can
// act on: a task that states no decision was assembled somewhere other than
// a survey of ours, and the token is worth printing for the others.
//
// Nothing a real survey writes is refused: every decision in the document is
// written by decisionToken from the reconcile the plane already ran.
func checkReconcileSurveyDecisions(report reconcileReport) error {
	stated := map[string]struct{}{
		decisionToken(github.PublishNeeded):    {},
		decisionToken(github.PublishSettled):   {},
		decisionToken(github.PublishInFlight):  {},
		decisionToken(github.PublishConflicts): {},
	}
	for _, task := range report.Tasks {
		if strings.TrimSpace(task.Decision) == "" {
			return fmt.Errorf("%w: %s states no decision",
				ErrReconcilePageInvalid, statedSurveyTask(task.Task))
		}
		if _, writes := stated[task.Decision]; !writes {
			return fmt.Errorf("%w: %s carries a decision this survey does not write: %s",
				ErrReconcilePageInvalid, statedSurveyTask(task.Task),
				strings.TrimSpace(task.Decision))
		}
	}
	return nil
}

// statedSurveyTask names a task for a refusal about a task the page would
// never have printed. A settled task's name is nowhere on the page, so an
// unnamed one is said to be unnamed rather than left as a gap in the
// sentence.
func statedSurveyTask(task string) string {
	task = strings.TrimSpace(task)
	if task == "" {
		return "an unnamed task"
	}
	return "task " + task
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
	publishedAs := make(map[int64]reconcileContestedRun, len(report.Tasks))
	for _, task := range report.Tasks {
		for _, run := range task.Published {
			if run.Identifier == ours {
				continue
			}
			contested[run.ID] = run.Identifier
			publishedAs[run.ID] = reconcileContestedRun{
				ID:   run.ID,
				Task: task.Task,
				Name: task.Name,
			}
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
	if err := checkSurveyGrouping(report.ContestedStanding, "under a name we plan",
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
		}); err != nil {
		return err
	}
	return checkContestedRunNames(report.ContestedStanding, publishedAs)
}

// checkContestedRunNames refuses a contested group that names a run under a
// check run, or a task, the listing it was derived from does not give it.
//
// The grouping check reads WHERE a run stands. The two words beside it on
// the line are the ones the reader acts on: "under a name we plan, foreign:
// #7 under ra8ci / build (build)" sends an operator to a check run called
// "ra8ci / build" and tells them which of our plans wanted that name. Both
// are copied out of the task whose listing carried the run, and until now
// nothing read them back against it. A document whose standing says
// "ra8ci / lint" over a listing that published the run under
// "ra8ci / build" sends the reader looking for a check run no task in the
// document plans, with the page's own authority behind it, and every other
// check on this page passes: the identifier agrees, the counts are derived
// from the tasks, and the name is nowhere in them.
//
// The listing is the authority, not the standing. contestedStandings builds
// each grouped run out of the task it was published under, so the listing is
// where the word came from and the standing is the copy.
//
// The check run is read before the task, the order #1649 pinned for the two
// blank cases: the check run is what the section is for and what the reader
// opens, and the task only says which of our plans wanted the name.
//
// Both are matched exactly, with no trimming and no folding of case. A
// check run name is GitHub's own string and it is what a reader types into
// the Checks tab to find it, so "ra8ci / Build" and "ra8ci / build" are two
// different answers to the question this line exists to answer. That is the
// deliberate contrast with a commit, which the pages match without casing
// because a SHA is a number written down.
//
// Nothing the surveying command writes is refused: both words are copied
// from the task in the same walk that groups the run.
func checkContestedRunNames(
	standings []reconcileContestedStanding,
	publishedAs map[int64]reconcileContestedRun,
) error {
	for _, standing := range standings {
		for _, run := range standing.Runs {
			published, listed := publishedAs[run.ID]
			if !listed {
				// Unreachable through the page: the grouping
				// check above refuses a run the listing does
				// not carry, and it refuses it as that.
				continue
			}
			if run.Name != published.Name {
				return fmt.Errorf("%w: under a name we plan, run %d is stated under %s and the listing publishes it under %s",
					ErrReconcilePageInvalid, run.ID,
					statedSurveyCheckRun(run.Name), statedSurveyCheckRun(published.Name))
			}
			if run.Task != published.Task {
				return fmt.Errorf("%w: under a name we plan, run %d is stated under %s and the listing publishes it under %s",
					ErrReconcilePageInvalid, run.ID,
					statedSurveyTask(run.Task), statedSurveyTask(published.Task))
			}
		}
	}
	return nil
}

// statedSurveyCheckRun names a check run for a refusal, the sibling of
// statedSurveyTask. A blank one is said to be unnamed rather than left as a
// gap in a sentence whose whole subject is which name to go and look at.
func statedSurveyCheckRun(name string) string {
	name = strings.TrimSpace(name)
	if name == "" {
		return "an unnamed check run"
	}
	return "the check run " + name
}

// checkSurveyGrouping reads one grouping against the listing it is derived
// from. The two groupings carry different run records and are checked the
// same way, so the walk is written once and handed what it needs: the runs
// on a standing, the standing's identifier, and where a run actually stands.
//
// An empty group is refused rather than skipped. Both groupings leave a
// standing out when no run carries it, so an empty one is a group about
// nothing, and it would print as an identifier with an empty list after it.
//
// A run's number is read before anything else about it, because the number
// is the whole of what the page hands the reader: both grouping sections
// print their runs as "#7", and the reader's next move is to open that run.
// A run numbered zero prints as "#0", which is a run to go and look at that
// nobody can look at, and a number is what every other answer about the run
// is keyed by. This is the rule the candidate page keeps for a candidate
// the survey numbers zero.
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
			if run <= 0 {
				return fmt.Errorf("%w: %s a run grouped under %s is numbered %d",
					ErrReconcilePageInvalid, grouping, identifier, run)
			}
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
