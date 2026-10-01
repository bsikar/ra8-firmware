// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// checkCaptureStampSitsInTheAttempt holds an artifact's capture time to the
// attempt the manifest is closing.
//
// ArtifactManifest.Validate refuses a zero CapturedAt and says nothing else
// about it, because the wire contract has no attempt window to compare it
// against. The store does. By the time a close reaches this point the attempt
// row is locked and carries both ends of the window the grant allowed:
// issued_at, when the plane handed the work out, and deadline_at, the last
// moment the work was allowed to be running. Nothing compared the guest's
// stamp with either of them, so a manifest could close an artifact captured
// last year or a decade from now and the plane would file it.
//
// The stamp is read, not decoration. It is the only time in agent_artifacts
// that comes from the guest rather than from clock_timestamp() on the plane,
// and it is what an operator reads to place an artifact against the run that
// produced it: which attempt of a flaky task produced this core dump, whether
// a map file predates the build beside it. A stamp outside the attempt
// answers those questions with a capture the attempt cannot have made, and
// answering them wrongly is worse than refusing.
//
// The allowance is agentEvidenceGrace on both sides, the same sixty seconds
// agentEvidenceWindow already gives a late upload. The two clocks here are
// the plane's, which stamped issued_at and deadline_at, and the guest's,
// which read CapturedAt, so the window has to be widened by what those two
// can disagree about; this package already fixes that quantity, and stating a
// second allowance beside it would be two answers to one question.
//
// Both bounds are the attempt's own, not the plane's clock. A capture may
// legitimately sit well in the past relative to now, since an agent uploads
// evidence after the work finished, and agentEvidenceWindow already refuses
// an upload arriving after the attempt's grace has run out. What this rule
// adds is the other end: a stamp from before the attempt existed, or from
// after the last moment it could still have been running.
func checkCaptureStampSitsInTheAttempt(manifest protocol.ArtifactManifest, attempt agentAttempt) error {
	captured := manifest.CapturedAt.UTC()
	if captured.Before(attempt.IssuedAt.Add(-agentEvidenceGrace)) {
		return fmt.Errorf("%w: artifact was captured before the attempt was issued", ErrConflict)
	}
	if captured.After(attempt.DeadlineAt.Add(agentEvidenceGrace)) {
		return fmt.Errorf("%w: artifact was captured after the attempt's deadline", ErrConflict)
	}
	return nil
}
