// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
)

// CheckRunReconciler lists what this plane has already published on a commit.
// It deliberately decides nothing: a listing is evidence, and what to do about
// it needs the run a caller meant to post beside it.
//
// This file is that decision, and nothing here speaks to GitHub. A publisher
// whose write came back uncertain reads the commit, asks this file what the
// listing means for the run in hand, and gets one of four answers: post it,
// leave it alone, wait, or stop and fetch a person.
//
// The rule the whole file serves is the implementation contract's: an
// uncertain write is reconciled, never blindly repeated. Every answer short of
// PublishNeeded therefore withholds the second post, and the two that withhold
// it without the run being settled say plainly which kind of unsettled they
// are, because waiting for a run that is still executing and arguing with a
// run that already disagrees are different pieces of work.

// PublishDecision is what a listing means for one intended check run.
type PublishDecision int

const (
	// PublishNeeded means nothing on the commit carries this name, so the
	// run has not been published and posting it is safe.
	PublishNeeded PublishDecision = iota
	// PublishSettled means this exact run is already on the commit. A
	// second post would add a duplicate run under a name branch
	// protection may one day require, which is the blind repeat the
	// contract forbids.
	PublishSettled
	// PublishInFlight means a run under this name has not finished. It is
	// the ordinary shape of an uncertain write: the post landed and the
	// answer did not. The caller waits and reads the commit again.
	PublishInFlight
	// PublishConflicts means a finished run under this name says something
	// other than the run in hand. Nothing here can tell which is right, so
	// it is reported rather than resolved.
	PublishConflicts
)

// String names the decision for errors and logs.
func (d PublishDecision) String() string {
	switch d {
	case PublishNeeded:
		return "needed"
	case PublishSettled:
		return "settled"
	case PublishInFlight:
		return "in flight"
	case PublishConflicts:
		return "conflicts"
	default:
		return fmt.Sprintf("PublishDecision(%d)", int(d))
	}
}

var (
	// ErrReconcileCommitMismatch is returned when the listing is about a
	// different commit than the run. A publish reconciled against another
	// commit's runs is worse than no reconciliation: it would report a run
	// that was never posted for the commit in hand.
	ErrReconcileCommitMismatch = errors.New("check run listing is about a different commit")
	// ErrReconcileRunIncomplete is returned for an intended run this
	// package would refuse to publish anyway.
	ErrReconcileRunIncomplete = errors.New("incomplete intended check run")
)

// ReconciledPublish is the decision and the runs it was made from.
type ReconciledPublish struct {
	Decision PublishDecision
	// Existing are the runs already on the commit under this name, in the
	// order the listing carried them. There may be more than one, and a
	// caller that wants to explain the decision needs all of them.
	Existing []PublishedCheckRun
}

// Repeat reports whether the caller should post the run. It is true for
// exactly one decision, so a caller cannot reach a second post by reading the
// answer loosely.
func (r ReconciledPublish) Repeat() bool { return r.Decision == PublishNeeded }

// ReconcilePublish decides what a commit's published runs mean for the run a
// caller meant to post.
//
// The match is on the commit and the check run name. The contract also names
// the App and an external ID; nothing this plane posts carries an external ID
// yet, and the name is already this plane's own by construction, so matching
// on it is the whole of what the tree can currently check. A run under one of
// our names posted by something else is a collision an operator has to settle,
// and it lands here as a conflict rather than as a run to post over.
//
// A shadow run is matched on its title as well as its conclusion. Every
// completed shadow run reports neutral whatever the task did and carries the
// observed conclusion in its title, so two shadow runs about opposite
// observations agree on conclusion alone. Comparing only the conclusion there
// would call a run settled that says the opposite of the one in hand.
func ReconcilePublish(intended TaskCheckRun, published PublishedCheckRuns) (ReconciledPublish, error) {
	if intended.Name == "" || !validCommitSHA(intended.HeadSHA) ||
		intended.Status != "completed" || intended.Conclusion == "" {
		return ReconciledPublish{}, ErrReconcileRunIncomplete
	}
	if intended.Mode != ModeShadow && intended.Mode != ModeAuthoritative {
		return ReconciledPublish{}, fmt.Errorf("%w: %s", ErrInvalidCheckRunMode, intended.Mode)
	}
	if !equalCommit(published.HeadSHA, intended.HeadSHA) {
		return ReconciledPublish{}, fmt.Errorf("%w: listing is for %q, run is for %q",
			ErrReconcileCommitMismatch, published.HeadSHA, intended.HeadSHA)
	}
	existing := published.Named(intended.Name)
	if len(existing) == 0 {
		return ReconciledPublish{Decision: PublishNeeded}, nil
	}
	// An unfinished run is reported before any disagreement is: what a run
	// still executing will conclude is not known yet, so calling it a
	// conflict would send an operator to settle an argument that may not
	// exist. Waiting is the answer that costs nothing.
	for _, run := range existing {
		if run.Status != "completed" {
			return ReconciledPublish{Decision: PublishInFlight, Existing: existing}, nil
		}
	}
	for _, run := range existing {
		if !sameCheckRun(intended, run) {
			return ReconciledPublish{Decision: PublishConflicts, Existing: existing}, nil
		}
	}
	return ReconciledPublish{Decision: PublishSettled, Existing: existing}, nil
}

// sameCheckRun reports whether a published run says what the intended run
// says. The title is part of the comparison for a shadow run, where the
// conclusion is neutral whatever was observed.
func sameCheckRun(intended TaskCheckRun, published PublishedCheckRun) bool {
	if published.Mode != intended.Mode || published.Conclusion != intended.Conclusion {
		return false
	}
	if intended.Mode == ModeShadow {
		return published.Title == intended.Title
	}
	return true
}

// equalCommit compares two commit SHAs, which GitHub renders in either case.
func equalCommit(left, right string) bool {
	if len(left) != len(right) {
		return false
	}
	for index := 0; index < len(left); index++ {
		if lower(left[index]) != lower(right[index]) {
			return false
		}
	}
	return true
}

func lower(char byte) byte {
	if char >= 'A' && char <= 'Z' {
		return char + ('a' - 'A')
	}
	return char
}
