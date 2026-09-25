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
	// Unclaimed are the runs among Existing that this plane did not post:
	// they carry another deployment's external identifier, or none at
	// all. It is empty unless the decision is PublishConflicts, and it is
	// what separates the two conflicts an operator can meet: our own runs
	// disagreeing about one commit, and somebody else publishing under a
	// name of ours. They are different pieces of work, so the answer says
	// which one it is rather than leaving it to be guessed from the runs.
	Unclaimed []PublishedCheckRun
}

// Repeat reports whether the caller should post the run. It is true for
// exactly one decision, so a caller cannot reach a second post by reading the
// answer loosely.
func (r ReconciledPublish) Repeat() bool { return r.Decision == PublishNeeded }

// ReconcilePublish decides what a commit's published runs mean for the run a
// caller meant to post.
//
// The match is on the commit, the check run name and this plane's own external
// identifier, which is the set the implementation contract names. A name is
// public: anything holding a checks:write token on the repository can post
// under one of ours, and while nothing this plane posted carried an identifier
// a run somebody else left under our name was indistinguishable from our own.
// It is distinguishable now, and a run under our name that this plane did not
// post is reported as a conflict rather than waited for or posted over,
// because it is an argument about who owns the name and no amount of waiting
// settles it.
//
// A run carrying no identifier at all is unclaimed for the same reason: it is
// either a run posted before this plane wrote the field or a run posted by
// something else, and both are states an operator has to see rather than have
// answered for them by a match that cannot tell them apart.
//
// A shadow run of ours is matched on its title as well as its conclusion. Every
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
	// Who posted a run is settled before what it says, and before whether
	// it has finished. A run this plane did not post is not ours to wait
	// for: it concludes on somebody else's schedule and never answers the
	// question the caller came with, so reporting it as a write still in
	// flight would send an operator away to wait for an answer that is
	// not coming.
	var unclaimed []PublishedCheckRun
	for _, run := range existing {
		if !SameCheckRunExternalID(intended, run) {
			unclaimed = append(unclaimed, run)
		}
	}
	if len(unclaimed) > 0 {
		return ReconciledPublish{Decision: PublishConflicts, Existing: existing, Unclaimed: unclaimed}, nil
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
// says. It is asked only of runs this plane posted, so it judges what a run
// says and never who left it there. The title is part of the comparison for a
// shadow run, where the conclusion is neutral whatever was observed.
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
