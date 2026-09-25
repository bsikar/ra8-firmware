// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"sort"
	"strings"
)

// shadow_compare.go grades a pairing of one task outcome against one Actions
// job, and says plainly that the pairing is the caller's statement rather than
// this package's inference. Nothing in the tree records which Actions job
// covers which catalog task, so until something states it there is no way to
// assemble the pairings a comparison is made from.
//
// This file is that statement and the assembly around it. A correspondence is
// a reviewed map from catalog task to workflow job, checked against the
// catalog it claims to cover, and Collect turns it plus the two sides' raw
// outcomes into the observation set CompareShadowRun grades.
//
// The correspondence is data an operator declares, not a constant here,
// because the workflow's job names are the workflow's to choose and they
// change without ra8ci. What is fixed here is what a usable correspondence
// looks like and what happens to an outcome it does not cover.

var (
	// ErrShadowCorrespondenceInvalid is returned for a correspondence that
	// cannot be used: no pairs at all, a task name outside the catalog's
	// rule, a task the catalog does not carry, an unusable job name.
	ErrShadowCorrespondenceInvalid = errors.New("invalid shadow task correspondence")
	// ErrShadowCorrespondenceAmbiguous is returned when one Actions job is
	// named as covering two catalog tasks. A job reports one conclusion,
	// so a failure under it cannot be attributed to either task, and
	// grading both against it would report a conflict for a task that may
	// well have passed.
	ErrShadowCorrespondenceAmbiguous = errors.New("shadow correspondence gives one Actions job two tasks")
	// ErrShadowTaskNotCorrespondent is returned when this plane reported a
	// task the correspondence does not cover. It is refused rather than
	// dropped: silently leaving an outcome out shrinks the evidence the
	// required-check decision rests on without saying so.
	ErrShadowTaskNotCorrespondent = errors.New("shadow correspondence names no Actions job for task")
)

// ShadowCorrespondence states which Actions workflow job covers which catalog
// task. It is immutable after construction.
type ShadowCorrespondence struct {
	jobs  map[string]string
	tasks []string
}

// NewShadowCorrespondence validates a declared correspondence against the
// catalog task names it claims to cover.
//
// knownTasks is the catalog's own name list. It is required, not optional: a
// correspondence checked against nothing is unchecked, and a pair naming a
// task no reviewed definition carries would publish no check run and compare
// against nothing, which is a deployment mistake worth catching before a
// shadow run rather than after one.
func NewShadowCorrespondence(pairs map[string]string, knownTasks []string) (*ShadowCorrespondence, error) {
	if len(pairs) == 0 {
		return nil, fmt.Errorf("%w: no pairs", ErrShadowCorrespondenceInvalid)
	}
	if len(knownTasks) == 0 {
		return nil, fmt.Errorf("%w: no catalog task names to check against", ErrShadowCorrespondenceInvalid)
	}
	known := make(map[string]bool, len(knownTasks))
	for _, name := range knownTasks {
		known[name] = true
	}
	correspondence := &ShadowCorrespondence{
		jobs:  make(map[string]string, len(pairs)),
		tasks: make([]string, 0, len(pairs)),
	}
	covered := make(map[string]string, len(pairs))
	for task, job := range pairs {
		if !validCheckRunTask(task) {
			return nil, fmt.Errorf("%w: task %q", ErrShadowCorrespondenceInvalid, task)
		}
		if !known[task] {
			return nil, fmt.Errorf("%w: catalog carries no task %q", ErrShadowCorrespondenceInvalid, task)
		}
		if !validActionsJobName(job) {
			return nil, fmt.Errorf("%w: Actions job %q for task %q", ErrShadowCorrespondenceInvalid, job, task)
		}
		if other, taken := covered[job]; taken {
			return nil, fmt.Errorf("%w: job %q covers %q and %q",
				ErrShadowCorrespondenceAmbiguous, job, other, task)
		}
		covered[job] = task
		correspondence.jobs[task] = job
		correspondence.tasks = append(correspondence.tasks, task)
	}
	sort.Strings(correspondence.tasks)
	return correspondence, nil
}

// Job returns the Actions job declared to cover a task.
func (c *ShadowCorrespondence) Job(task string) (string, bool) {
	job, ok := c.jobs[task]
	return job, ok
}

// Tasks returns the covered catalog task names in order.
func (c *ShadowCorrespondence) Tasks() []string {
	tasks := make([]string, len(c.tasks))
	copy(tasks, c.tasks)
	return tasks
}

// PlaneOutcome is what this plane observed for one task on one commit. Observed
// is the conclusion ObservedConclusion returns for the task's terminal state,
// never the neutral conclusion a shadow check run is posted with.
type PlaneOutcome struct {
	Task     string
	HeadSHA  string
	Observed string
}

// ActionsOutcome is what one workflow job concluded for one commit. An empty
// Conclusion is a job that has not completed.
type ActionsOutcome struct {
	Job        string
	HeadSHA    string
	Conclusion string
}

// ShadowCollection is the observation set for one commit, plus what the
// correspondence covers that this commit did not exercise.
type ShadowCollection struct {
	HeadSHA string
	// Observations are ready for CompareShadowRun, ordered by task name.
	Observations []ShadowObservation
	// NotRun are covered tasks this plane reported no outcome for. A task
	// selection that skipped them is a normal commit, not a gap in the
	// evidence, so they are counted here rather than paired against
	// nothing.
	NotRun []string
}

// Collect assembles the pairings for one commit from what each side reported.
//
// Three cases are decided here rather than left to the caller, because each of
// them is a way a shadow comparison quietly says less than it appears to:
//
// A task this plane reported that the correspondence does not cover is
// refused. It is evidence with nowhere to go, and dropping it would shrink the
// set the decision rests on silently.
//
// An Actions job outside the correspondence is ignored without complaint. The
// workflow legitimately runs jobs ra8ci has no task for, and a correspondence
// is a statement about tasks, never a claim to describe the whole workflow.
//
// A covered task whose job Actions did not report is kept, with an empty
// conclusion. CompareShadowRun grades that as indeterminate, which is the true
// answer: the pairing was never judged. Leaving it out would let a report be
// clean because a comparison was missing.
func (c *ShadowCorrespondence) Collect(plane []PlaneOutcome, actions []ActionsOutcome) (ShadowCollection, error) {
	if len(plane) == 0 {
		return ShadowCollection{}, fmt.Errorf("%w: no plane outcomes", ErrShadowObservationInvalid)
	}
	collection := ShadowCollection{Observations: make([]ShadowObservation, 0, len(plane))}
	concluded := make(map[string]string, len(actions))
	seenJob := make(map[string]bool, len(actions))
	for _, outcome := range actions {
		if !validActionsJobName(outcome.Job) {
			return ShadowCollection{}, fmt.Errorf("%w: Actions job %q", ErrShadowObservationInvalid, outcome.Job)
		}
		if !validCommitSHA(outcome.HeadSHA) {
			return ShadowCollection{}, fmt.Errorf("%w: head SHA %q for job %q",
				ErrShadowObservationInvalid, outcome.HeadSHA, outcome.Job)
		}
		if outcome.Conclusion != "" && !actionsConclusions[outcome.Conclusion] {
			return ShadowCollection{}, fmt.Errorf("%w: Actions conclusion %q for job %q",
				ErrShadowObservationInvalid, outcome.Conclusion, outcome.Job)
		}
		if err := collection.adoptHead(outcome.HeadSHA); err != nil {
			return ShadowCollection{}, err
		}
		if seenJob[outcome.Job] {
			return ShadowCollection{}, fmt.Errorf("%w: Actions job %q reported twice",
				ErrShadowSetAmbiguous, outcome.Job)
		}
		seenJob[outcome.Job] = true
		concluded[outcome.Job] = outcome.Conclusion
	}
	reported := make(map[string]bool, len(plane))
	for _, outcome := range plane {
		if !validCheckRunTask(outcome.Task) {
			return ShadowCollection{}, fmt.Errorf("%w: task %q", ErrShadowObservationInvalid, outcome.Task)
		}
		if !validCommitSHA(outcome.HeadSHA) {
			return ShadowCollection{}, fmt.Errorf("%w: head SHA %q for task %q",
				ErrShadowObservationInvalid, outcome.HeadSHA, outcome.Task)
		}
		if !checkRunConclusion(outcome.Observed) {
			return ShadowCollection{}, fmt.Errorf("%w: observed conclusion %q for task %q",
				ErrShadowObservationInvalid, outcome.Observed, outcome.Task)
		}
		if err := collection.adoptHead(outcome.HeadSHA); err != nil {
			return ShadowCollection{}, err
		}
		if reported[outcome.Task] {
			return ShadowCollection{}, fmt.Errorf("%w: task %q reported twice",
				ErrShadowSetAmbiguous, outcome.Task)
		}
		reported[outcome.Task] = true
		job, covered := c.Job(outcome.Task)
		if !covered {
			return ShadowCollection{}, fmt.Errorf("%w: %q", ErrShadowTaskNotCorrespondent, outcome.Task)
		}
		collection.Observations = append(collection.Observations, ShadowObservation{
			Task:              outcome.Task,
			HeadSHA:           outcome.HeadSHA,
			Observed:          outcome.Observed,
			ActionsJob:        job,
			ActionsConclusion: concluded[job],
		})
	}
	for _, task := range c.tasks {
		if !reported[task] {
			collection.NotRun = append(collection.NotRun, task)
		}
	}
	sort.Slice(collection.Observations, func(i, j int) bool {
		return collection.Observations[i].Task < collection.Observations[j].Task
	})
	return collection, nil
}

// adoptHead keeps a collection to one commit. A set spanning two commits is
// the same caller mistake CompareShadowRun refuses, caught a step earlier so
// the pairing that crossed commits is named.
func (s *ShadowCollection) adoptHead(sha string) error {
	if s.HeadSHA == "" {
		s.HeadSHA = sha
		return nil
	}
	if s.HeadSHA != sha {
		return fmt.Errorf("%w: %s and %s", ErrShadowHeadMismatch, s.HeadSHA, sha)
	}
	return nil
}

// validActionsJobName states what a usable job name is. The bound is the one
// internal/demand applies to Event.JobName; the rest refuses a name that would
// read as a different job than the workflow runs: surrounding space that does
// not survive a round trip, and control characters that would break the report
// a person reads the comparison from.
func validActionsJobName(value string) bool {
	if value == "" || len(value) > maxActionsJobName {
		return false
	}
	if strings.TrimSpace(value) != value {
		return false
	}
	for _, char := range value {
		if char < 0x20 || char == 0x7f {
			return false
		}
	}
	return true
}
