// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"strings"
)

// SameCheckRunExternalID and PublishedByThisPlane both answer yes or no: does
// this run carry the identifier this plane computes for that name on that
// commit. Everything that is not a match is one answer, and the reconciliation
// treats it as one: reconcile_publish.go collects it into Unclaimed, the
// unplanned listing reports ours false, and an operator reading either is told
// only that the run is not ours.
//
// It is not one state. A run carrying no identifier at all was posted before
// this plane wrote the field, or by something that does not write one. A run
// carrying an identifier in a shape this plane never writes was posted by
// somebody else entirely, under a name of ours. A run carrying this plane's
// shape at a version this build does not derive is our own work from a
// deployment that computes identifiers differently. And a run carrying this
// plane's current shape whose digest is for other work is the one that reads
// like all the others and means something quite different: the run was ours,
// derived for a different name or a different commit, and it is sitting on
// this commit under this name. A check run's name can be edited after it is
// published and its identifier cannot, so a renamed run of ours lands here;
// so does a run reconciled against the earlier head of a re-used branch.
//
// This file keeps those apart. It decides nothing and it withholds nothing
// from the existing answers: PublishedByThisPlane is defined in terms of it,
// so one derivation serves both and the yes/no answer cannot drift from the
// longer one.

// externalIDPrefix opens every identifier this plane writes. The version
// follows it, so the prefix alone recognises the shape without claiming the
// value is one this build can recompute.
const externalIDPrefix = "ra8ci-"

// ErrExternalIDSubjectUnusable is returned when the standing cannot be asked:
// a run with no name, or a commit that is not a commit. The question is
// "was this posted for this name on this commit", and with either half
// missing there is nothing to compare against. Answering foreign there would
// report somebody else's run over a caller's own mistake.
var ErrExternalIDSubjectUnusable = errors.New("cannot judge a check run external identifier without a name and a commit")

// ExternalIDStanding is what one published run's external identifier says
// about who posted it.
//
// It is about the identifier and nothing else. A run's status, conclusion,
// title and summary are a different question, asked elsewhere: a run can be
// ours and disagree, or be a stranger's and agree, and reading either from
// the standing would be reading it from the wrong field.
type ExternalIDStanding int

const (
	// ExternalIDAbsent means the run carries no identifier. It is either
	// a run posted before this plane wrote the field or a run posted by
	// something that writes none, and a listing cannot tell those apart.
	ExternalIDAbsent ExternalIDStanding = iota
	// ExternalIDForeign means the identifier is not in a shape this
	// plane writes. Something else posted under a name of ours, which is
	// an argument about who owns the name.
	ExternalIDForeign
	// ExternalIDSuperseded means the identifier is this plane's shape at
	// a version this build does not derive. The run is ours, from a
	// deployment that computes identifiers differently, and no amount of
	// comparing settles it here: the two derivations have to be
	// reconciled by whoever changed one.
	ExternalIDSuperseded
	// ExternalIDOtherSubject means the identifier is this plane's
	// current shape and is not the one this name on this commit
	// computes. The run was posted by this plane for other work and is
	// on this commit under this name now.
	ExternalIDOtherSubject
	// ExternalIDOurs means the identifier is exactly the one this name
	// on this commit computes.
	ExternalIDOurs
)

// String names the standing for reports and errors.
func (s ExternalIDStanding) String() string {
	switch s {
	case ExternalIDAbsent:
		return "absent"
	case ExternalIDForeign:
		return "foreign"
	case ExternalIDSuperseded:
		return "superseded"
	case ExternalIDOtherSubject:
		return "other subject"
	case ExternalIDOurs:
		return "ours"
	default:
		return fmt.Sprintf("ExternalIDStanding(%d)", int(s))
	}
}

// Ours reports whether the standing is the one match. It is true for exactly
// one value, so a caller cannot reach a claim of ownership by reading the
// answer loosely.
func (s ExternalIDStanding) Ours() bool { return s == ExternalIDOurs }

// ExternalIDStandingOf reports what a published run's external identifier says
// about who posted it, for one commit.
//
// The subject is the run's own name and the commit given, which is the pair
// the identifier is derived from. It is asked this way rather than against an
// intended run because the interesting cases are the runs no plan claims,
// where there is no intended run to ask about.
//
// A digest that does not match is never inverted to say which name or commit
// it was derived for. The derivation is one way, so the honest answer is that
// the value is this plane's and is not this subject's; which subject it
// belongs to is a question for whoever holds the other commit.
func ExternalIDStandingOf(published PublishedCheckRun, headSHA string) (ExternalIDStanding, error) {
	if published.Name == "" || !validCommitSHA(headSHA) {
		return ExternalIDAbsent, fmt.Errorf("%w: name %q commit %q",
			ErrExternalIDSubjectUnusable, published.Name, headSHA)
	}
	if published.ExternalID == "" {
		return ExternalIDAbsent, nil
	}
	version, ok := externalIDVersionOf(published.ExternalID)
	if !ok {
		return ExternalIDForeign, nil
	}
	if version != externalIDVersion {
		return ExternalIDSuperseded, nil
	}
	identifier, err := CheckRunExternalID(TaskCheckRun{Name: published.Name, HeadSHA: headSHA})
	if err != nil {
		return ExternalIDAbsent, fmt.Errorf("%w: name %q commit %q",
			ErrExternalIDSubjectUnusable, published.Name, headSHA)
	}
	if identifier != published.ExternalID {
		return ExternalIDOtherSubject, nil
	}
	return ExternalIDOurs, nil
}

// externalIDVersionOf returns the version segment of an identifier written in
// this plane's shape, and whether the value is in that shape at all.
//
// The shape is the prefix, a version, a separator and the digest, and the
// digest has to be exactly the encoder's output: externalIDDigits of lower
// case hexadecimal. An identifier spelled in upper case is not a spelling this
// plane produces, so it is somebody else's value however much it resembles
// ours, and reading it as ours at another version would file a stranger's run
// under our own history.
func externalIDVersionOf(identifier string) (string, bool) {
	if !strings.HasPrefix(identifier, externalIDPrefix) {
		return "", false
	}
	separator := strings.LastIndexByte(identifier, '-')
	if separator < len(externalIDPrefix) {
		return "", false
	}
	version, digest := identifier[:separator], identifier[separator+1:]
	if len(digest) != externalIDDigits || !lowerHex(digest) {
		return "", false
	}
	if version == externalIDPrefix {
		return "", false
	}
	return version, true
}

// lowerHex reports whether every character is a lower case hexadecimal digit.
func lowerHex(value string) bool {
	for index := 0; index < len(value); index++ {
		char := value[index]
		if !(char >= '0' && char <= '9' || char >= 'a' && char <= 'f') {
			return false
		}
	}
	return true
}
