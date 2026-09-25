// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"sort"
)

// #1481 is explicit that ra8ci runs in shadow mode first and that conclusions
// are "compared against Actions over representative pull requests before any
// required check moves". check_runs.go states what a shadow run may carry and
// check_run_publisher.go posts it; this file is the comparison itself, and it
// is the evidence the decision to move a required check is made on.
//
// The question that decision turns on is not "did the two agree word for
// word". It is "would ra8ci have blocked a pull request Actions let through,
// or let through one Actions blocked". Two conclusions can differ and mean the
// same thing to branch protection (failure and timed_out both refuse a merge,
// success and skipped both satisfy one), and the difference between those two
// kinds of disagreement is the difference between a note and a reason to stop.
// So a comparison here is graded by gating effect, not by string equality
// alone, and the two are reported separately.

const (
	// maxActionsJobName bounds the Actions job name a pairing names. It is
	// the limit internal/demand already applies to Event.JobName, restated
	// because a pairing is validated here without an Event in hand.
	maxActionsJobName = 255
)

var (
	// ErrShadowObservationInvalid is returned for a pairing that cannot be
	// compared: an unnamed task, a head SHA that is not a commit, a
	// conclusion outside the vocabulary either side speaks.
	ErrShadowObservationInvalid = errors.New("invalid shadow comparison observation")
	// ErrShadowSetAmbiguous is returned when one task is paired twice for
	// the same commit. Picking a winner would report a comparison nobody
	// made, so the set is refused instead.
	ErrShadowSetAmbiguous = errors.New("shadow comparison set names a task twice")
	// ErrShadowHeadMismatch is returned when a set spans more than one head
	// commit. A report answers "is ra8ci ready to gate this commit", so a
	// set covering two commits is a caller mistake, not a wider report.
	ErrShadowHeadMismatch = errors.New("shadow comparison set spans more than one head commit")
)

// actionsConclusions is the vocabulary the Actions side speaks, taken from
// internal/demand, which validates exactly this set on a completed event.
// "stale" is in it and is not a verdict: internal/demand/reconcile.go writes it
// when the plane never saw the job end and the forge can no longer say how it
// did. Comparing against it would manufacture a disagreement out of an absence,
// so it is reported as indeterminate instead.
var actionsConclusions = map[string]bool{
	"success":         true,
	"failure":         true,
	"cancelled":       true,
	"skipped":         true,
	"timed_out":       true,
	"neutral":         true,
	"action_required": true,
	"stale":           true,
}

// actionsNonVerdicts are the Actions values that state no outcome. An empty
// conclusion is a job that has not completed, and "stale" is a job whose
// outcome was lost. Neither is evidence for or against ra8ci.
var actionsNonVerdicts = map[string]bool{
	"":      true,
	"stale": true,
}

// ShadowVerdict grades one pairing.
type ShadowVerdict int

const (
	// ShadowIndeterminate means Actions stated no outcome to compare
	// against. It is the zero value deliberately: a pairing that was never
	// judged must not read as agreement.
	ShadowIndeterminate ShadowVerdict = iota
	// ShadowAgreed means both sides reported the same conclusion.
	ShadowAgreed
	// ShadowDivergent means the conclusions differ but branch protection
	// would treat them the same way.
	ShadowDivergent
	// ShadowConflicting means the two sides disagree about whether the
	// pull request may merge. These are the pairings that hold a required
	// check where it is.
	ShadowConflicting
)

// String names the verdict for reports and errors.
func (v ShadowVerdict) String() string {
	switch v {
	case ShadowIndeterminate:
		return "indeterminate"
	case ShadowAgreed:
		return "agreed"
	case ShadowDivergent:
		return "divergent"
	case ShadowConflicting:
		return "conflicting"
	default:
		return fmt.Sprintf("ShadowVerdict(%d)", int(v))
	}
}

// ShadowObservation pairs one task outcome this plane observed with what
// Actions concluded for the same work on the same commit.
//
// The pairing is the caller's statement, not this package's inference. Nothing
// in the tree records which Actions job covers which catalog task, and
// inventing a correspondence here (by name similarity, say) would compare two
// unrelated results and report the answer as evidence. Whoever runs the
// comparison names the pair; this file only grades it.
type ShadowObservation struct {
	// Task is the catalog task name, the one CheckRunName publishes under.
	Task string
	// HeadSHA is the commit both sides judged.
	HeadSHA string
	// Observed is the conclusion this plane saw, the value
	// ObservedConclusion returns. In shadow mode this is what was posted in
	// the title, never the neutral conclusion that was posted as the run's
	// own.
	Observed string
	// ActionsJob is the workflow job the caller says covers this task.
	ActionsJob string
	// ActionsConclusion is what that job concluded. Empty means it has not
	// completed.
	ActionsConclusion string
}

// ShadowComparison is one graded pairing.
type ShadowComparison struct {
	Task              string
	HeadSHA           string
	ActionsJob        string
	Observed          string
	ActionsConclusion string
	Verdict           ShadowVerdict
	// PlaneBlocks and ActionsBlocks are what branch protection would do
	// with each side's conclusion. Both are false on an indeterminate
	// pairing, where there is no Actions verdict to gate on.
	PlaneBlocks   bool
	ActionsBlocks bool
}

// ShadowReport is the comparison for one commit.
type ShadowReport struct {
	HeadSHA       string
	Comparisons   []ShadowComparison
	Agreed        int
	Divergent     int
	Conflicting   int
	Indeterminate int
}

// Clean reports whether this commit gives no reason to hold the required check
// where it is: every pairing was judged, and none of them disagreed about
// whether the pull request may merge.
//
// A divergent pairing does not make a report unclean. It is a vocabulary
// difference branch protection cannot see, worth an operator's eye and counted
// separately for exactly that reason, but it is not evidence that moving a
// required check would change an outcome. An indeterminate pairing does make a
// report unclean: a comparison that was never made is not one that passed.
func (r ShadowReport) Clean() bool {
	return r.Conflicting == 0 && r.Indeterminate == 0
}

// CompareShadowRun grades a commit's pairings and returns them ordered by task
// name, so two passes over the same pull request are diffable.
//
// It reads nothing and posts nothing. The comparison that decides whether ra8ci
// may gate a merge is worth being a pure function of what both sides reported,
// so the decision can be re-made later from the same inputs.
func CompareShadowRun(observations []ShadowObservation) (ShadowReport, error) {
	if len(observations) == 0 {
		return ShadowReport{}, fmt.Errorf("%w: no observations", ErrShadowObservationInvalid)
	}
	report := ShadowReport{Comparisons: make([]ShadowComparison, 0, len(observations))}
	seen := make(map[string]bool, len(observations))
	for _, observation := range observations {
		if err := observation.validate(); err != nil {
			return ShadowReport{}, err
		}
		if report.HeadSHA == "" {
			report.HeadSHA = observation.HeadSHA
		} else if observation.HeadSHA != report.HeadSHA {
			return ShadowReport{}, fmt.Errorf("%w: %s and %s",
				ErrShadowHeadMismatch, report.HeadSHA, observation.HeadSHA)
		}
		if seen[observation.Task] {
			return ShadowReport{}, fmt.Errorf("%w: %q", ErrShadowSetAmbiguous, observation.Task)
		}
		seen[observation.Task] = true
		report.Comparisons = append(report.Comparisons, observation.compare())
	}
	sort.Slice(report.Comparisons, func(i, j int) bool {
		return report.Comparisons[i].Task < report.Comparisons[j].Task
	})
	for _, comparison := range report.Comparisons {
		switch comparison.Verdict {
		case ShadowAgreed:
			report.Agreed++
		case ShadowDivergent:
			report.Divergent++
		case ShadowConflicting:
			report.Conflicting++
		default:
			report.Indeterminate++
		}
	}
	return report, nil
}

// validate refuses a pairing this file cannot grade honestly, rather than
// grading it anyway. Every refusal names what was wrong with it.
func (o ShadowObservation) validate() error {
	if !validCheckRunTask(o.Task) {
		return fmt.Errorf("%w: task %q", ErrShadowObservationInvalid, o.Task)
	}
	if !validCommitSHA(o.HeadSHA) {
		return fmt.Errorf("%w: head SHA %q", ErrShadowObservationInvalid, o.HeadSHA)
	}
	if o.ActionsJob == "" || len(o.ActionsJob) > maxActionsJobName {
		return fmt.Errorf("%w: Actions job %q", ErrShadowObservationInvalid, o.ActionsJob)
	}
	if !checkRunConclusion(o.Observed) {
		return fmt.Errorf("%w: observed conclusion %q", ErrShadowObservationInvalid, o.Observed)
	}
	if o.ActionsConclusion != "" && !actionsConclusions[o.ActionsConclusion] {
		return fmt.Errorf("%w: Actions conclusion %q", ErrShadowObservationInvalid, o.ActionsConclusion)
	}
	return nil
}

// compare grades one validated pairing.
func (o ShadowObservation) compare() ShadowComparison {
	comparison := ShadowComparison{
		Task:              o.Task,
		HeadSHA:           o.HeadSHA,
		ActionsJob:        o.ActionsJob,
		Observed:          o.Observed,
		ActionsConclusion: o.ActionsConclusion,
	}
	if actionsNonVerdicts[o.ActionsConclusion] {
		comparison.Verdict = ShadowIndeterminate
		return comparison
	}
	comparison.PlaneBlocks = blockingConclusion(o.Observed)
	comparison.ActionsBlocks = blockingConclusion(o.ActionsConclusion)
	switch {
	case o.Observed == o.ActionsConclusion:
		comparison.Verdict = ShadowAgreed
	case comparison.PlaneBlocks == comparison.ActionsBlocks:
		comparison.Verdict = ShadowDivergent
	default:
		comparison.Verdict = ShadowConflicting
	}
	return comparison
}

// blockingConclusion reports what branch protection would do with a
// conclusion, on either side. It is the same rule TaskCheckRun.Blocking
// applies, stated over a bare conclusion so an Actions value can be graded by
// it too.
func blockingConclusion(conclusion string) bool { return !nonBlockingConclusions[conclusion] }

// checkRunConclusion reports whether a value is one this plane ever observes.
// It is the image of observedConclusions, so a state added to the task machine
// widens what a comparison accepts through one map rather than two.
func checkRunConclusion(value string) bool {
	for _, conclusion := range observedConclusions {
		if conclusion == value {
			return true
		}
	}
	return false
}
