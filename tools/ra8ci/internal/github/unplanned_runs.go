// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
)

// CheckRunReconciler lists every run on a commit that sits inside one of this
// plane's two namespaces, and ReconcilePublish answers about one intended run
// at a time. PublishedCheckRuns.Named is the only way to ask what is on the
// commit, and it answers only for a name a caller already holds.
//
// That leaves one state nothing in the tree can see. A task retired from the
// catalog, renamed, or dropped from a document still has its check runs on
// every commit they were published to, under a name no plan asks about any
// more. The reconciliation walks the plan, finds each planned name accounted
// for, and reports a settled commit while a run under a retired name sits
// beside them, occupying a name branch protection could still be pointed at.
//
// This file is the other direction: given a listing and the names a plan
// claims, which runs does nothing in the plan account for. It decides nothing
// and it removes nothing. A stale run is a fact about the commit an operator
// has to see; which of them should be left alone, re-run under a current name,
// or argued about with whoever posted it is not a judgement a listing can
// make.

var (
	// ErrUnplannedPlanEmpty is returned when no planned name is given.
	// An empty plan is never read as "nothing is claimed": every run on
	// the commit would answer as unplanned, which is the loudest possible
	// report of a caller that simply failed to build its plan.
	ErrUnplannedPlanEmpty = errors.New("no planned check run names to account for the commit's runs")
	// ErrUnplannedPlanName is returned for a blank planned name. A blank
	// name claims no run and matches none, so accepting it would quietly
	// let a plan under-claim and report somebody's live run as stale.
	ErrUnplannedPlanName = errors.New("invalid planned check run name")
)

// UnplannedRuns reports the runs in one commit's listing that no planned check
// run name claims.
//
// The names are matched exactly. A GitHub check run name is the literal string
// it was posted under, branch protection requires it literally, and two names
// differing only in case or spacing are two names; matching them loosely here
// would report a live run as stale, or worse, account for a retired one and
// say nothing.
//
// The answer keeps the listing's order, which PublishedRuns sorts by name and
// then by identifier, so the same commit read twice produces the same
// document. Both namespaces are answered over together: the listing carries
// the mode of every run it kept, and a stale shadow run is exactly as
// invisible to the plan as a stale authoritative one.
//
// A run that has not finished is reported like any other. Whether a run is
// accounted for is a question about its name, and a run still executing under
// a name nothing plans is the same unaccounted-for run it will be when it
// concludes.
func UnplannedRuns(published PublishedCheckRuns, planned []string) ([]PublishedCheckRun, error) {
	if len(planned) == 0 {
		return nil, ErrUnplannedPlanEmpty
	}
	claimed := make(map[string]bool, len(planned))
	for _, name := range planned {
		if name == "" {
			return nil, fmt.Errorf("%w: %q", ErrUnplannedPlanName, name)
		}
		claimed[name] = true
	}
	unplanned := []PublishedCheckRun{}
	for _, run := range published.Runs {
		if !claimed[run.Name] {
			unplanned = append(unplanned, run)
		}
	}
	return unplanned, nil
}

// PublishedByThisPlane reports whether a run on the commit carries the external
// identifier this plane computes for that name and commit.
//
// It is the same derivation SameCheckRunExternalID makes, asked without an
// intended run beside it, which is the only way it can be asked about a run no
// plan claims. The identifier is derived from the check run name and the
// commit and from nothing else, so a run under a retired task name still
// answers true: it was ours, posted under a name we have since stopped
// planning, and that is a different piece of work from a run somebody else
// left under a name of ours.
//
// A run carrying no identifier answers false, for the reason
// SameCheckRunExternalID gives: it is either a run posted before this plane
// wrote the field or a run posted by something else, and a listing cannot tell
// those apart.
func PublishedByThisPlane(published PublishedCheckRun, headSHA string) bool {
	if published.ExternalID == "" {
		return false
	}
	identifier, err := CheckRunExternalID(TaskCheckRun{Name: published.Name, HeadSHA: headSHA})
	if err != nil {
		return false
	}
	return identifier == published.ExternalID
}
