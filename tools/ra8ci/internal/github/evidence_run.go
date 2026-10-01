// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"strings"
)

// RunsOn (#1590) lists the workflow runs GitHub recorded against a commit and
// deliberately does not say which of them is the evidence, and Outcomes
// (#1578) grades a run whose ID somebody already decided on. Between the two
// the operator gathering #1481's evidence still chose by eye: a commit carries
// the checks run, a docs run, a labeller and whatever a release workflow left
// behind, and the run ID that went into the comparison document was the one
// that looked right in the web interface.
//
// SelectEvidenceRun answers that question once, in one place, against rules a
// test can pin. It is pure: it speaks to no GitHub, holds no token, and
// decides nothing about what the run concluded. Everything it will not answer
// it refuses by name, because an evidence document built on the wrong run is
// exactly the error the whole shadow comparison exists to catch, and a
// refusal an operator reads beats a selection nobody can check.

var (
	// ErrEvidenceWorkflowUnnamed is returned when no workflow was named.
	// Which workflow carries the shadow tasks is the deployment's fact, not
	// something this file may guess at from a listing: picking "the only
	// completed run" would grade a commit against its docs workflow on the
	// morning the checks workflow failed to start.
	ErrEvidenceWorkflowUnnamed = errors.New("no workflow named to select a run from")

	// ErrEvidenceRunNotFound is returned when the commit carries no run of
	// the named workflow at all. It names the workflows that were listed,
	// so an operator who typed the name differently can see what GitHub
	// actually calls it.
	ErrEvidenceRunNotFound = errors.New("this commit carries no run of that workflow")

	// ErrEvidenceRunIncomplete is returned when a run of the named workflow
	// is still executing. It is kept apart from a commit with no such run:
	// one is waiting, the other is looking somewhere else, and they are
	// different work for the person holding the listing.
	ErrEvidenceRunIncomplete = errors.New("a run of that workflow has not completed")

	// ErrEvidenceRunUndecided is returned when every run of the named
	// workflow finished without deciding anything: cancelled, skipped,
	// stale or waiting on somebody's approval. Such a run has job
	// conclusions Outcomes would happily grade, and grading them records a
	// comparison against an answer nobody ever got.
	ErrEvidenceRunUndecided = errors.New("no run of that workflow decided anything")

	// ErrEvidenceRunAmbiguous is returned when two runs of the named
	// workflow both decided the commit.
	ErrEvidenceRunAmbiguous = errors.New("this commit carries two decided runs of that workflow")
)

// SelectEvidenceRun picks the one run on a commit whose job conclusions are
// the Actions half of the shadow comparison.
//
// The workflow is named by the caller and matched exactly. A workflow name is
// what its author wrote at the top of the file, two workflows in one
// repository may differ by nothing more than case or spacing, and a loose
// match here would select the wrong one silently. A name that does not match
// is refused with the names the listing carried, which is the same information
// a looser match would have used and leaves the choice with the operator.
//
// Ambiguity is refused rather than resolved. CommitWorkflowRuns records that
// GitHub listed the runs newest first, but that is GitHub's word about an
// order rather than a field on the listing this plane can check, and a
// re-run's evidence and the original run's evidence disagree precisely when
// the comparison matters most.
func SelectEvidenceRun(listed CommitWorkflowRuns, workflow string) (CommitWorkflowRun, error) {
	if !validCommitSHA(listed.HeadSHA) {
		return CommitWorkflowRun{}, fmt.Errorf("%w: %q is not a commit", ErrInvalidCheckRunSHA, listed.HeadSHA)
	}
	if strings.TrimSpace(workflow) == "" {
		return CommitWorkflowRun{}, ErrEvidenceWorkflowUnnamed
	}

	named := []CommitWorkflowRun{}
	for _, run := range listed.Runs {
		if run.Workflow == workflow {
			named = append(named, run)
		}
	}
	if len(named) == 0 {
		return CommitWorkflowRun{}, fmt.Errorf("%w: %s carries %s",
			ErrEvidenceRunNotFound, listed.HeadSHA, describeWorkflowsListed(listed.Runs))
	}

	// A run still executing refuses the selection even when another run of
	// the same workflow has already decided the commit. A re-run in flight
	// is the state in which the two answers are most likely to differ, and
	// banking the finished one there records evidence the run nobody waited
	// for would have overturned. Outcomes refuses an unfinished run for the
	// same reason (#1578).
	for _, run := range named {
		if !run.Completed() {
			return CommitWorkflowRun{}, fmt.Errorf("%w: run %d is %q",
				ErrEvidenceRunIncomplete, run.ID, run.Status)
		}
	}

	decided := []CommitWorkflowRun{}
	for _, run := range named {
		if decidedConclusion(run.Conclusion) {
			decided = append(decided, run)
		}
	}
	switch len(decided) {
	case 0:
		return CommitWorkflowRun{}, fmt.Errorf("%w: run %d concluded %q",
			ErrEvidenceRunUndecided, named[0].ID, named[0].Conclusion)
	case 1:
		return decided[0], nil
	default:
		return CommitWorkflowRun{}, fmt.Errorf("%w: runs %s",
			ErrEvidenceRunAmbiguous, describeRunIDs(decided))
	}
}

// decidedConclusion reports whether a completed run answered the question the
// comparison asks. success, failure, neutral and timed_out are answers: the
// tasks ran and something came of them. cancelled, skipped, stale and
// action_required are not, whatever their jobs recorded on the way past.
//
// It lives here rather than as a method beside Completed because whether a
// conclusion decided anything is a question about selecting evidence, not
// about what GitHub listed, and the reader's job is to report GitHub's word
// without grading it (#1590).
func decidedConclusion(conclusion string) bool {
	switch conclusion {
	case "success", "failure", "neutral", "timed_out":
		return true
	default:
		return false
	}
}

// describeWorkflowsListed names the workflows a commit carried, each once and
// in listing order, so a refusal says what is there rather than only what is
// missing. A commit with no runs at all says so in words.
func describeWorkflowsListed(runs []CommitWorkflowRun) string {
	if len(runs) == 0 {
		return "no workflow runs"
	}
	seen := map[string]bool{}
	names := []string{}
	for _, run := range runs {
		if run.Workflow == "" || seen[run.Workflow] {
			continue
		}
		seen[run.Workflow] = true
		names = append(names, fmt.Sprintf("%q", run.Workflow))
	}
	if len(names) == 0 {
		return "runs of no named workflow"
	}
	return strings.Join(names, ", ")
}

// describeRunIDs names the runs an ambiguous selection could not choose
// between, so the operator can go and look at both.
func describeRunIDs(runs []CommitWorkflowRun) string {
	ids := make([]string, 0, len(runs))
	for _, run := range runs {
		ids = append(ids, fmt.Sprintf("%d", run.ID))
	}
	return strings.Join(ids, " and ")
}
