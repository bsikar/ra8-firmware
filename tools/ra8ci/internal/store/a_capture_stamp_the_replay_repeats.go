// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// captureStampResolution is what the column holding a capture stamp can keep.
// agent_artifacts.captured_at is timestamptz, which Postgres stores to the
// microsecond, so a stamp read back is the one that was written rounded to
// this. An honest agent reads its clock in nanoseconds (time.Now in the
// collector), so comparing the two at full precision would refuse the very
// replay this rule exists to admit.
const captureStampResolution = time.Microsecond

// sameCaptureStamp reports whether a manifest repeats the capture stamp
// already on file, at the resolution the column keeps.
//
// A stored stamp of zero means the plane is not holding one to compare
// against. The migration's CHECK ties captured_at to closed_at, so a closed
// artifact always has one, but a predicate that refuses on evidence it does
// not have would turn a read that returned nothing into a conflict the agent
// cannot resolve by retrying. Absent, it says nothing.
func sameCaptureStamp(manifest protocol.ArtifactManifest, stored time.Time) bool {
	if stored.IsZero() {
		return true
	}
	return manifest.CapturedAt.UTC().Truncate(captureStampResolution).
		Equal(stored.UTC().Truncate(captureStampResolution))
}

// checkReplayRepeatsTheCaptureStamp holds a replayed manifest to the capture
// time the close already filed.
//
// captured_at is written once, by the close that accepted the artifact, and
// never again: the UPDATE runs only on the accepted path, so a second manifest
// carrying a different stamp leaves the first one on file and is still
// answered ArtifactDuplicate. That word means the far end re-presented
// evidence already on file, unchanged, and here it would be saying so about
// the one field the two manifests disagree on.
//
// The stamp is not decoration. It is when the guest captured the bytes, which
// is what an operator reads to place an artifact against the run that produced
// it, and it is the only time in the row that comes from the guest rather than
// from clock_timestamp() on the plane. Two manifests that disagree about it
// describe two captures, and only one of them is stored.
func checkReplayRepeatsTheCaptureStamp(manifest protocol.ArtifactManifest, artifact heldArtifact) error {
	if !sameCaptureStamp(manifest, artifact.CapturedAt) {
		return fmt.Errorf("%w: artifact was closed on another capture time", ErrConflict)
	}
	return nil
}
