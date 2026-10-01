// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"sort"
)

// required_checks.go plans the gate from the deployment's MODE alone: an
// authoritative deployment proposes requiring every task it covers. That is
// #1481's hold stated once, and on its own it is not the hold the issue asks
// for. The issue defers the required-check move until conclusions "have been
// compared against Actions over representative pull requests", and a deployment
// flipped to authoritative on its first morning passes the mode test having
// compared nothing at all. The evidence that answers the plural question landed
// in shadow_evidence.go, and nothing read it back into the plan.
//
// This file is that join. The mode says whether the gate may move at all; the
// evidence says which tasks have earned a place on it. A task the evidence does
// not back is WITHHELD and named, never quietly added and never quietly dropped,
// because a gate nobody decided to add is the failure this whole issue is
// arranged around.
//
// Like required_checks.go, the plan is computed and never applied. Nothing here
// speaks to GitHub.

// ErrRequiredCheckEvidenceEmpty is returned when the readiness answer covers no
// task at all. An empty readiness cannot be told apart from a caller whose
// collection failed, and planning from it proposes tearing down every context
// this plane owns. Same refusal, and the same reason, as ErrNoRequiredCheckTasks.
var ErrRequiredCheckEvidenceEmpty = errors.New("shadow readiness covers no task")

// WithheldReason says why the evidence did not back requiring a task's context.
// The two are separate because they are different work: one is a disagreement
// somebody has to explain, the other is pull requests nobody has run yet.
type WithheldReason int

const (
	// WithheldInsufficient is a task with no conflict and fewer graded
	// commits than the threshold asked for. Nothing is wrong with it; the
	// evidence is simply not in yet.
	WithheldInsufficient WithheldReason = iota + 1
	// WithheldConflicting is a task where ra8ci and Actions disagreed at
	// least once about whether a pull request may merge. Requiring it
	// would hand merge decisions to a plane known to have made a different
	// one.
	WithheldConflicting
)

// String names the reason for a report an operator reads.
func (r WithheldReason) String() string {
	switch r {
	case WithheldInsufficient:
		return "insufficient"
	case WithheldConflicting:
		return "conflicting"
	default:
		return "unknown"
	}
}

// WithheldTask is one task the mode would have put on the gate and the evidence
// would not.
type WithheldTask struct {
	// Task is the catalog task.
	Task string
	// Context is the required-check name that was not proposed, so the
	// operator reads the same string branch protection would have carried.
	Context string
	// Reason is why the evidence did not back it.
	Reason WithheldReason
	// AlreadyRequired says the gate carries this context today. The plan
	// keeps it: withholding decides whether this plane ASKS for a new
	// gate, never whether protection an operator already put there comes
	// off. It is reported because a gate running ahead of the evidence is
	// worth knowing about.
	AlreadyRequired bool
}

// EvidenceBackedPlan is the gate plan with the evidence decision attached.
type EvidenceBackedPlan struct {
	// Plan is what required_checks.go would plan, with the unbacked
	// additions taken out of Add.
	Plan RequiredCheckPlan
	// Threshold is the number of graded commits the readiness answer was
	// read at, kept with the plan because the same evidence plans
	// differently at a different threshold.
	Threshold int
	// Withheld names the tasks the evidence did not back, ordered by task.
	Withheld []WithheldTask
}

// Withholds reports whether the evidence held anything back.
func (p EvidenceBackedPlan) Withholds() bool { return len(p.Withheld) > 0 }

// PlanRequiredChecksFromEvidence plans the gate for the tasks the accumulated
// shadow evidence backs at this readiness answer's threshold.
//
// Only a Ready task may be proposed for the gate. A conflicting task is the one
// ra8ci has already been wrong about, and an insufficient one has not been
// compared often enough for anyone to say; requiring either is the move #1481
// exists to defer. Both are named rather than dropped, so the report says which
// tasks the deployment is waiting on and why.
func PlanRequiredChecksFromEvidence(mode CheckRunMode, readiness ShadowReadiness, currentlyRequired []string) (EvidenceBackedPlan, error) {
	if mode != ModeShadow && mode != ModeAuthoritative {
		return EvidenceBackedPlan{}, fmt.Errorf("%w: %s", ErrInvalidCheckRunMode, mode)
	}
	// A zero-value ShadowReadiness carries threshold 0, which is what a
	// caller that never asked Readiness() looks like. Refused for the same
	// reason Readiness refuses it: an answer computed with no evidence
	// required is not a readiness answer.
	if readiness.Threshold < 1 {
		return EvidenceBackedPlan{}, fmt.Errorf("%w: %d", ErrShadowEvidenceThresholdInvalid, readiness.Threshold)
	}

	covered := make([]string, 0, len(readiness.Ready)+len(readiness.Conflicting)+len(readiness.Insufficient))
	covered = append(covered, readiness.Ready...)
	covered = append(covered, readiness.Conflicting...)
	covered = append(covered, readiness.Insufficient...)
	if len(covered) == 0 {
		return EvidenceBackedPlan{}, ErrRequiredCheckEvidenceEmpty
	}
	sort.Strings(covered)

	// One place decides what a usable task name, a usable context and a
	// usable set are. A task named in two of the readiness lists arrives
	// here as one task twice and is refused as ambiguous, rather than this
	// file picking whichever list it read first.
	plan, err := PlanRequiredChecks(mode, covered, currentlyRequired)
	if err != nil {
		return EvidenceBackedPlan{}, err
	}

	backed := EvidenceBackedPlan{Plan: plan, Threshold: readiness.Threshold, Withheld: []WithheldTask{}}
	if mode == ModeShadow {
		// The mode already refused every addition, so there is nothing
		// for the evidence to withhold. Reporting the covered tasks as
		// withheld here would credit the evidence with a decision the
		// mode made.
		return backed, nil
	}

	ready := make(map[string]bool, len(readiness.Ready))
	for _, task := range readiness.Ready {
		ready[task] = true
	}
	reasons := make(map[string]WithheldReason, len(readiness.Conflicting)+len(readiness.Insufficient))
	for _, task := range readiness.Conflicting {
		reasons[task] = WithheldConflicting
	}
	for _, task := range readiness.Insufficient {
		reasons[task] = WithheldInsufficient
	}

	// Add and Keep are drawn from the tasks passed in, so every context in
	// them has a task here.
	byContext := make(map[string]string, len(covered))
	for _, task := range covered {
		name, err := CheckRunName(ModeAuthoritative, task)
		if err != nil {
			return EvidenceBackedPlan{}, err
		}
		byContext[name] = task
	}

	add := make([]string, 0, len(plan.Add))
	for _, context := range plan.Add {
		task := byContext[context]
		if ready[task] {
			add = append(add, context)
			continue
		}
		backed.Withheld = append(backed.Withheld, WithheldTask{
			Task:    task,
			Context: context,
			Reason:  reasons[task],
		})
	}
	backed.Plan.Add = add

	for _, context := range plan.Keep {
		task := byContext[context]
		if ready[task] {
			continue
		}
		backed.Withheld = append(backed.Withheld, WithheldTask{
			Task:            task,
			Context:         context,
			Reason:          reasons[task],
			AlreadyRequired: true,
		})
	}

	sort.Slice(backed.Withheld, func(i, j int) bool {
		return backed.Withheld[i].Task < backed.Withheld[j].Task
	})
	return backed, nil
}
