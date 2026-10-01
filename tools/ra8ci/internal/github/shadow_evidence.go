// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"sort"
)

// #1481 holds the required-check move until shadow conclusions have been
// "compared against Actions over representative pull requests". shadow_compare.go
// grades ONE commit and shadow_report_render.go prints that one commit's page.
// Neither can answer the question the issue actually asks, because that question
// is plural: one clean pull request is not evidence that ra8ci may gate the
// repository, and eighty clean pull requests are not evidence for a task none of
// them exercised.
//
// This file accumulates graded commits into the evidence the decision reads. It
// counts per task, because branch protection requires a context per task and a
// repository-wide tally would let a task compared once ride on comparisons of
// other tasks. It reads nothing and posts nothing: the decision to move a
// required check is worth being a pure function of the reports it was made from,
// so it can be re-made later from the same inputs.

var (
	// ErrShadowEvidenceEmpty is returned when there is nothing to
	// accumulate. An answer computed from no reports would say every task
	// is un-exercised, which is indistinguishable from a caller that
	// failed to collect anything.
	ErrShadowEvidenceEmpty = errors.New("no shadow reports to accumulate")
	// ErrShadowEvidenceRepeatedCommit is returned when one commit appears
	// twice. Re-grading a pull request produces the same observations
	// again, and counting them twice inflates the evidence a threshold is
	// read against without anyone having looked at a second pull request.
	ErrShadowEvidenceRepeatedCommit = errors.New("shadow evidence names one commit twice")
	// ErrShadowEvidenceReportInvalid is returned for a report this file
	// cannot accumulate honestly: no head commit, no comparisons, or a
	// comparison belonging to some other commit.
	ErrShadowEvidenceReportInvalid = errors.New("invalid shadow report")
	// ErrShadowEvidenceThresholdInvalid is returned for a threshold below
	// one. A readiness answer computed with no evidence required is not a
	// readiness answer.
	ErrShadowEvidenceThresholdInvalid = errors.New("shadow evidence threshold must be at least one")
)

// TaskEvidence is everything the accumulated reports say about one task.
//
// Graded is the count that a threshold is read against: the commits on which
// both sides stated an outcome and the pairing was judged. Indeterminate
// pairings are counted but are not graded commits, for the reason
// shadow_compare.go makes ShadowIndeterminate the zero value: a pairing nobody
// judged must not read as one that passed.
type TaskEvidence struct {
	Task string
	// Observed is how many accumulated commits paired this task at all.
	Observed int
	// Graded is Agreed + Divergent + Conflicting: the commits that
	// produced a verdict. A conflict is a verdict. It is the one that
	// holds the required check where it is, and leaving it out of the
	// count would say a task nobody could agree about had been graded
	// on fewer commits than it was. Observed is Graded + Indeterminate:
	// the pairings that produced no verdict are counted and named, not
	// graded.
	Graded int
	// Agreed, Divergent, Conflicting and Indeterminate are the verdicts
	// this task collected, in shadow_compare.go's vocabulary.
	Agreed        int
	Divergent     int
	Conflicting   int
	Indeterminate int
	// ConflictingCommits names the commits where ra8ci and Actions
	// disagreed about whether the pull request may merge, in the order
	// the reports were given. These are the pairings an operator has to
	// explain, so the answer names them rather than counting them.
	ConflictingCommits []string
	// IndeterminateCommits names the commits where this task was paired
	// but nobody judged the pairing, in the order the reports were
	// given. Indeterminate above counts them, and a count is not enough
	// to act on: a task held short of its threshold moves forward by
	// going back to the pull requests whose Actions side stated no
	// outcome, and until they are named an operator holding this answer
	// cannot tell which of the accumulated commits those were.
	IndeterminateCommits []string
}

// ShadowEvidence is the accumulated comparison across commits.
type ShadowEvidence struct {
	// Commits are the head SHAs accumulated, in the order given. The
	// order pull requests were observed in is the caller's record of what
	// happened; re-sorting by SHA would invent an order that means
	// nothing.
	Commits []string
	// Tasks are ordered by task name, so two passes are diffable, the
	// convention shadow_compare.go and shadow_correspondence.go follow.
	Tasks []TaskEvidence
	// UngradedCommits names the accumulated commits that graded nothing:
	// every task they paired came back indeterminate, so they moved no
	// task one step closer to its threshold. They are kept apart from
	// Commits rather than dropped from it, because both facts are true
	// and each is read for a different reason: Commits is the record of
	// what was looked at, and this is the part of that record that
	// counted for nothing. A caller holding only the first reads the
	// breadth of the evidence as wider than it is, which is the one
	// mistake this accumulation exists to prevent.
	UngradedCommits []string
}

// ShadowReadiness is the answer to "may a required check move for this task",
// read at one threshold. Every task the evidence covers appears in exactly one
// of the three lists, each ordered by task name.
type ShadowReadiness struct {
	// Threshold is the number of graded commits asked for, kept with the
	// answer because the same evidence gives a different answer at a
	// different threshold.
	Threshold int
	// Ready are the tasks graded on at least Threshold commits with no
	// conflict on any of them.
	Ready []string
	// Conflicting are the tasks where ra8ci and Actions disagreed about a
	// merge at least once. They are named separately from Insufficient
	// because they need different work: a disagreement to explain, not
	// more pull requests to wait for.
	Conflicting []string
	// Insufficient are the tasks with no conflict and fewer than
	// Threshold graded commits.
	Insufficient []string
	// Shortfall is one entry per Insufficient task, in the same order,
	// saying how far short it is. A name on its own says a task is not
	// ready; it does not say whether one more pull request finishes it
	// or twenty do, and that is the difference between waiting for the
	// next merge and going looking for commits that exercise the task.
	//
	// It covers the insufficient tasks and nothing else. A conflicting
	// task carries no shortfall on purpose: writing one would read as
	// "grade this many more and it is ready", and a conflict is not
	// cleared by more commits.
	Shortfall []TaskShortfall
}

// TaskShortfall is how far one insufficient task is from its threshold.
type TaskShortfall struct {
	Task string
	// Graded is what the accumulated reports produced a verdict on.
	Graded int
	// Remaining is Threshold minus Graded, always at least one: a task
	// with nothing remaining is ready and is not reported here.
	Remaining int
}

// Settled reports whether every covered task is ready at this threshold.
func (r ShadowReadiness) Settled() bool {
	return len(r.Conflicting) == 0 && len(r.Insufficient) == 0 && len(r.Ready) > 0
}

// AccumulateShadowEvidence folds graded commits into one per-task ledger.
//
// It refuses rather than resolves: a repeated commit, a report with no head
// commit, and a comparison carrying some other commit's SHA are all caller
// mistakes whose silent acceptance would overstate the evidence.
func AccumulateShadowEvidence(reports []ShadowReport) (ShadowEvidence, error) {
	if len(reports) == 0 {
		return ShadowEvidence{}, ErrShadowEvidenceEmpty
	}
	evidence := ShadowEvidence{
		Commits:         make([]string, 0, len(reports)),
		UngradedCommits: []string{},
	}
	seenCommit := make(map[string]bool, len(reports))
	byTask := make(map[string]*TaskEvidence)
	for _, report := range reports {
		if !validCommitSHA(report.HeadSHA) {
			return ShadowEvidence{}, fmt.Errorf("%w: head %q is not a commit",
				ErrShadowEvidenceReportInvalid, report.HeadSHA)
		}
		if len(report.Comparisons) == 0 {
			return ShadowEvidence{}, fmt.Errorf("%w: %s compares nothing",
				ErrShadowEvidenceReportInvalid, report.HeadSHA)
		}
		if seenCommit[report.HeadSHA] {
			return ShadowEvidence{}, fmt.Errorf("%w: %s", ErrShadowEvidenceRepeatedCommit, report.HeadSHA)
		}
		seenCommit[report.HeadSHA] = true
		evidence.Commits = append(evidence.Commits, report.HeadSHA)

		seenTask := make(map[string]bool, len(report.Comparisons))
		gradedHere := 0
		for _, comparison := range report.Comparisons {
			if comparison.HeadSHA != report.HeadSHA {
				return ShadowEvidence{}, fmt.Errorf("%w: %s carries a comparison of %s",
					ErrShadowEvidenceReportInvalid, report.HeadSHA, comparison.HeadSHA)
			}
			if comparison.Task == "" {
				return ShadowEvidence{}, fmt.Errorf("%w: %s compares an unnamed task",
					ErrShadowEvidenceReportInvalid, report.HeadSHA)
			}
			if seenTask[comparison.Task] {
				return ShadowEvidence{}, fmt.Errorf("%w: %q on %s",
					ErrShadowSetAmbiguous, comparison.Task, report.HeadSHA)
			}
			seenTask[comparison.Task] = true

			task := byTask[comparison.Task]
			if task == nil {
				task = &TaskEvidence{Task: comparison.Task}
				byTask[comparison.Task] = task
			}
			task.Observed++
			switch comparison.Verdict {
			case ShadowAgreed:
				task.Agreed++
				task.Graded++
				gradedHere++
			case ShadowDivergent:
				// A divergence is evidence. shadow_compare.go grades
				// by gating effect and Clean() already treats it as
				// clean, so withholding it here would hold a gate on a
				// difference branch protection cannot see.
				task.Divergent++
				task.Graded++
				gradedHere++
			case ShadowConflicting:
				task.Conflicting++
				task.Graded++
				gradedHere++
				task.ConflictingCommits = append(task.ConflictingCommits, report.HeadSHA)
			default:
				task.Indeterminate++
				task.IndeterminateCommits = append(task.IndeterminateCommits, report.HeadSHA)
			}
		}
		// A commit whose every pairing came back indeterminate is
		// recorded as one that graded nothing. It is NOT refused: a
		// pull request whose Actions side stated no outcome is an
		// ordinary thing to have looked at, and refusing the whole
		// accumulation over one would throw away the commits that did
		// grade. It is named instead, because the alternative is a
		// caller counting it as one of the representative pull
		// requests #1481 asks for.
		if gradedHere == 0 {
			evidence.UngradedCommits = append(evidence.UngradedCommits, report.HeadSHA)
		}
	}
	evidence.Tasks = make([]TaskEvidence, 0, len(byTask))
	for _, task := range byTask {
		evidence.Tasks = append(evidence.Tasks, *task)
	}
	sort.Slice(evidence.Tasks, func(i, j int) bool {
		return evidence.Tasks[i].Task < evidence.Tasks[j].Task
	})
	return evidence, nil
}

// Readiness partitions the covered tasks at one threshold.
//
// A single conflict holds a task however many agreements follow it. The
// question #1481 defers the required-check move on is whether ra8ci has ever
// disagreed with Actions about whether a pull request may merge, not what
// share of the time it agreed, and a ratio would let one unexplained conflict
// be outvoted by routine passes.
//
// A task the accumulated reports never mention does not appear here at all.
// This answers for the evidence in hand; naming the catalog tasks no pull
// request exercised is the caller's, and `ra8ci github required-checks`
// already reports uncovered tasks from the declared correspondence.
func (e ShadowEvidence) Readiness(threshold int) (ShadowReadiness, error) {
	if threshold < 1 {
		return ShadowReadiness{}, fmt.Errorf("%w: %d", ErrShadowEvidenceThresholdInvalid, threshold)
	}
	readiness := ShadowReadiness{
		Threshold:    threshold,
		Ready:        []string{},
		Conflicting:  []string{},
		Insufficient: []string{},
		Shortfall:    []TaskShortfall{},
	}
	for _, task := range e.Tasks {
		switch {
		case task.Conflicting > 0:
			readiness.Conflicting = append(readiness.Conflicting, task.Task)
		case task.Graded >= threshold:
			readiness.Ready = append(readiness.Ready, task.Task)
		default:
			readiness.Insufficient = append(readiness.Insufficient, task.Task)
			readiness.Shortfall = append(readiness.Shortfall, TaskShortfall{
				Task:      task.Task,
				Graded:    task.Graded,
				Remaining: threshold - task.Graded,
			})
		}
	}
	return readiness, nil
}
