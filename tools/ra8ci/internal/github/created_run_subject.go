// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import "fmt"

// GitHub is the record of what was published, so Publish reads the created
// run back before it reports anything as posted. The run that answer
// describes is identified the way every other file in this package
// identifies a check run: by its name and the commit it sits on. The
// identifier derivation takes those two and nothing else, the reconciler
// lists by them, and ReconcilePublish refuses a listing whose commit is not
// the intended run's.
//
// The read-back compared the name and the conclusion. It decoded the commit
// and the status and compared neither, so the one half of a run's identity
// that Publish cannot see for itself was the half it never checked. A
// response placing the run on another commit, or reporting it as still
// queued, was reported as a successful publish and its identifier handed
// back to the caller.
//
// What that costs is not the one run. The caller records the returned
// identifier beside the commit it meant to publish for, and every later
// reconciliation asks GitHub for that commit's runs: a run recorded on
// another commit is not in that listing, so the reconciliation answers
// PublishNeeded and the plane posts again, every pass, for a run that
// exists. A run recorded as queued is the mirror of it: the listing carries
// it, ReconcilePublish sees a status that is not completed and answers
// PublishInFlight, and an operator is sent to wait for an answer that has
// already been given. Both read as ordinary states rather than as a
// disagreement with GitHub about what was published.
//
// The echoed external identifier keeps the rule it already had, stated here
// beside the others rather than trailing them: an identifier naming another
// run is refused, an absent one is silence about the field. The distinction
// is not available to the name, the commit or the status, which GitHub
// answers with on every create; a response missing one of those is not
// silence, it is an answer this plane cannot place.
func checkCreatedRunIsTheRunPosted(created checkRunResponse, posted TaskCheckRun, externalID string) error {
	if created.Name != posted.Name {
		return fmt.Errorf("%w: GitHub recorded the run under %q, posted as %q",
			ErrCheckRunRejected, created.Name, posted.Name)
	}
	// The commit is compared case-insensitively, as it is everywhere else
	// in this package: GitHub renders a SHA in either case and the two
	// spellings are one commit. A blank one is not a spelling, it is an
	// answer that places the run nowhere, and the whole point of the
	// comparison is where the run landed.
	if !equalCommit(created.HeadSHA, posted.HeadSHA) {
		return fmt.Errorf("%w: GitHub recorded the run on commit %q, posted for %q",
			ErrCheckRunRejected, created.HeadSHA, posted.HeadSHA)
	}
	if created.Status != posted.Status {
		return fmt.Errorf("%w: GitHub recorded the run as %q, posted as %q",
			ErrCheckRunRejected, created.Status, posted.Status)
	}
	if created.Conclusion != posted.Conclusion {
		return fmt.Errorf("%w: GitHub recorded conclusion %q, posted %q",
			ErrCheckRunRejected, created.Conclusion, posted.Conclusion)
	}
	if created.ExternalID != "" && created.ExternalID != externalID {
		return fmt.Errorf("%w: GitHub recorded external id %q for %q", ErrCheckRunRejected,
			created.ExternalID, externalID)
	}
	return nil
}
