// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
)

// A check run posted by this plane carries an external identifier, which is
// the field GitHub keeps for the publisher's own name for the run.
//
// The implementation contract asks that an uncertain Checks API write be
// reconciled by listing check runs for "SHA/name/App/external ID". The
// reconciler (check_run_reconciler.go) and the decision beside it
// (reconcile_publish.go) both had to match on the commit and the name alone,
// because nothing this plane posted carried an external identifier at all.
// Matching on the name is enough to find a run under one of our namespaces,
// and not enough to say this deployment posted it: a name is public, and
// anything holding a checks:write token on the repository can post under it.
//
// The identifier here closes that: it is derived from the run itself, so two
// deployments publishing the same task on the same commit compute the same
// value and a run posted by something else does not. It is deliberately not a
// secret and not a nonce. A random identifier would be unrecoverable after the
// write whose answer never arrived, which is precisely the case the
// reconciliation exists for, so the value has to be something a later
// reconciliation can recompute from the run in hand.

const (
	// externalIDVersion prefixes the identifier so a later change to what
	// goes into it is visible in the value rather than silently matching
	// nothing.
	externalIDVersion = "ra8ci-1"
	// externalIDDomain separates this derivation from any other use of
	// the same hash, and the null bytes separate the fields so no two
	// different runs can be spelled into one identifier.
	externalIDDomain = "ra8ci check run external id v1"
	// externalIDDigits is how much of the digest the identifier carries.
	// 32 hex digits is 128 bits, far more than a repository's check runs
	// need to stay distinct, and short enough to read in a listing.
	externalIDDigits = 32
)

// ErrCheckRunExternalIDUnbuildable is returned for a run whose name or commit
// this package would refuse to publish anyway.
var ErrCheckRunExternalIDUnbuildable = errors.New("cannot build a check run external identifier")

// CheckRunExternalID is this plane's own identifier for one task's check run
// on one commit.
//
// It is derived from the check run name and the commit, and from nothing else.
// The name already carries both the mode and the task, so an identifier built
// from it cannot disagree with the name it is posted beside, and the run's
// conclusion is deliberately left out: a run republished after a different
// observation is the same run, and the reconciliation has to recognise it in
// order to report the disagreement rather than post a second one.
//
// The commit is compared case-insensitively everywhere else in this package,
// so it is lowered before it is hashed and a SHA in either case produces one
// identifier.
func CheckRunExternalID(run TaskCheckRun) (string, error) {
	if run.Name == "" || !validCommitSHA(run.HeadSHA) {
		return "", fmt.Errorf("%w: name %q commit %q", ErrCheckRunExternalIDUnbuildable, run.Name, run.HeadSHA)
	}
	digest := sha256.Sum256([]byte(externalIDDomain + "\x00" + run.Name + "\x00" + lowerASCII(run.HeadSHA)))
	return externalIDVersion + "-" + hex.EncodeToString(digest[:])[:externalIDDigits], nil
}

// SameCheckRunExternalID reports whether a published run carries the
// identifier one intended run computes.
//
// A published run with no identifier is not a match. It is either a run posted
// before this field existed or a run posted by something else, and both are
// states an operator has to see rather than have answered for them.
func SameCheckRunExternalID(intended TaskCheckRun, published PublishedCheckRun) bool {
	if published.ExternalID == "" {
		return false
	}
	identifier, err := CheckRunExternalID(intended)
	if err != nil {
		return false
	}
	return identifier == published.ExternalID
}

// lowerASCII lowers a commit SHA, which GitHub renders in either case.
func lowerASCII(value string) string {
	lowered := []byte(value)
	for index := range lowered {
		lowered[index] = lower(lowered[index])
	}
	return string(lowered)
}
