// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// closedFixture is the plane's copy of an artifact that is already closed on
// the bytes payload carries, in final chunks.
func closedFixture(payload string, chunks int64) heldArtifact {
	return heldArtifact{Exists: true, Closed: true, StepKey: "build",
		TotalBytes: int64(len(payload)), Chunks: chunks, SHA256: digestOf(payload)}
}

// replayed answers what artifactClose says about a manifest arriving against
// an already closed artifact.
func replayed(manifest protocol.ArtifactManifest, artifact heldArtifact) (ArtifactOutcome, error) {
	return artifactClose(manifest, artifact, "")
}

func TestReplayOfTheSameManifestIsADuplicate(t *testing.T) {
	payload := "firstsecond"
	outcome, err := replayed(artifactManifestFixture(int64(len(payload)), 2, payload),
		closedFixture(payload, 2))
	if err != nil || outcome != ArtifactDuplicate {
		t.Fatalf("replay: outcome %q err %v", outcome, err)
	}
}

func TestReplayClaimingFewerChunksIsRefused(t *testing.T) {
	payload := "firstsecond"
	if _, err := replayed(artifactManifestFixture(int64(len(payload)), 1, payload),
		closedFixture(payload, 2)); !errors.Is(err, ErrConflict) {
		t.Fatalf("fewer chunks: want conflict, got %v", err)
	}
}

func TestReplayClaimingMoreChunksIsRefused(t *testing.T) {
	payload := "firstsecond"
	if _, err := replayed(artifactManifestFixture(int64(len(payload)), 5, payload),
		closedFixture(payload, 2)); !errors.Is(err, ErrConflict) {
		t.Fatalf("more chunks: want conflict, got %v", err)
	}
}

// The open path already refuses a chunk count that is not what was uploaded.
// This pins that the closed path now answers the same disagreement the same
// way, so arrival order does not decide the verdict.
func TestTheChunkCountIsJudgedOpenOrClosed(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 1, payload)
	open := heldArtifact{Exists: true, StepKey: "build",
		TotalBytes: int64(len(payload)), Chunks: 2}
	_, openErr := artifactClose(manifest, open, digestOf(payload))
	_, closedErr := replayed(manifest, closedFixture(payload, 2))
	if !errors.Is(openErr, ErrConflict) || !errors.Is(closedErr, ErrConflict) {
		t.Fatalf("open %v closed %v: both should be a conflict", openErr, closedErr)
	}
}

func TestReplayOnOtherBytesIsRefused(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, "other bytes entirely")
	if _, err := replayed(manifest, closedFixture(payload, 2)); !errors.Is(err, ErrConflict) {
		t.Fatalf("other digest: want conflict, got %v", err)
	}
}

func TestReplayOnAnotherTotalIsRefused(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload))+1, 2, payload)
	if _, err := replayed(manifest, closedFixture(payload, 2)); !errors.Is(err, ErrConflict) {
		t.Fatalf("other total: want conflict, got %v", err)
	}
}

func TestReplayOnOtherTruncationEvidenceIsRefused(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	manifest.Truncated = true
	if _, err := replayed(manifest, closedFixture(payload, 2)); !errors.Is(err, ErrConflict) {
		t.Fatalf("other truncation: want conflict, got %v", err)
	}
}

// A truncated artifact is still evidence: a close that agreed it was truncated
// replays as a duplicate like any other.
func TestATruncatedCloseReplaysAsADuplicate(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	manifest.Truncated = true
	artifact := closedFixture(payload, 2)
	artifact.Truncated = true
	outcome, err := replayed(manifest, artifact)
	if err != nil || outcome != ArtifactDuplicate {
		t.Fatalf("truncated replay: outcome %q err %v", outcome, err)
	}
}

// A replay naming another step is refused before the close comparison, by the
// step rule that already stood above it.
func TestReplayNamingAnotherStepIsStillRefused(t *testing.T) {
	payload := "firstsecond"
	artifact := closedFixture(payload, 2)
	artifact.StepKey = "test"
	if _, err := replayed(artifactManifestFixture(int64(len(payload)), 2, payload),
		artifact); !errors.Is(err, ErrConflict) {
		t.Fatalf("other step: want conflict, got %v", err)
	}
}

// The predicate itself, away from artifactClose, so the message each
// disagreement earns is pinned.
func TestClosedArtifactComparisonNamesWhatDisagreed(t *testing.T) {
	payload := "firstsecond"
	artifact := closedFixture(payload, 2)
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	if err := closedArtifactMatchesTheManifest(manifest, artifact); err != nil {
		t.Fatalf("agreeing manifest: %v", err)
	}
	counted := manifest
	counted.FinalSequence = 3
	err := closedArtifactMatchesTheManifest(counted, artifact)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("chunk count: want conflict, got %v", err)
	}
	if got := err.Error(); got == "" || !errors.Is(err, ErrConflict) {
		t.Fatalf("chunk count message: %q", got)
	}
}
