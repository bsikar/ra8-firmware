// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// closedAt is the plane's copy of an artifact closed on payload, with the
// capture stamp the accepted close filed.
func closedAtStamp(payload string, chunks int64, stamp time.Time) heldArtifact {
	artifact := closedFixture(payload, chunks)
	artifact.CapturedAt = stamp
	return artifact
}

func TestReplayRepeatingTheCaptureStampIsADuplicate(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	outcome, err := replayed(manifest, closedAtStamp(payload, 2, manifest.CapturedAt))
	if err != nil || outcome != ArtifactDuplicate {
		t.Fatalf("same stamp: outcome %q err %v", outcome, err)
	}
}

func TestReplayOnAnotherCaptureStampIsRefused(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	stored := manifest.CapturedAt.Add(time.Second)
	if _, err := replayed(manifest, closedAtStamp(payload, 2, stored)); !errors.Is(err, ErrConflict) {
		t.Fatalf("other stamp: want conflict, got %v", err)
	}
}

// The column keeps microseconds, so a stamp read back has lost whatever the
// guest's clock said below that. An honest replay must still be a duplicate.
func TestASubMicrosecondDifferenceIsTheSameStamp(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	manifest.CapturedAt = manifest.CapturedAt.Add(742 * time.Nanosecond)
	outcome, err := replayed(manifest, closedAtStamp(payload, 2, manifest.CapturedAt.Truncate(time.Microsecond)))
	if err != nil || outcome != ArtifactDuplicate {
		t.Fatalf("rounded stamp: outcome %q err %v", outcome, err)
	}
}

func TestAMicrosecondApartIsAnotherStamp(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	stored := manifest.CapturedAt.Add(time.Microsecond)
	if _, err := replayed(manifest, closedAtStamp(payload, 2, stored)); !errors.Is(err, ErrConflict) {
		t.Fatalf("one microsecond apart: want conflict, got %v", err)
	}
}

// One instant stated in two zones is one stamp, because the store writes and
// reads it in UTC.
func TestTheSameInstantInAnotherZoneIsTheSameStamp(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	elsewhere := time.FixedZone("UTC-7", -7*60*60)
	if !sameCaptureStamp(manifest, manifest.CapturedAt.In(elsewhere)) {
		t.Fatal("one instant in two zones should be one stamp")
	}
}

// A row with no stamp on file is not evidence of a disagreement. The
// migration ties captured_at to closed_at so a closed artifact always carries
// one, but the predicate must not turn a missing read into a conflict the
// agent cannot retry out of.
func TestNoStampOnFileSaysNothing(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	if !sameCaptureStamp(manifest, time.Time{}) {
		t.Fatal("absent stamp should not refuse")
	}
	outcome, err := replayed(manifest, closedFixture(payload, 2))
	if err != nil || outcome != ArtifactDuplicate {
		t.Fatalf("absent stamp: outcome %q err %v", outcome, err)
	}
}

// The stamp is judged after the bytes: a replay that disagrees about both
// reports the evidence, which is the larger disagreement.
func TestTheBytesAreReportedBeforeTheStamp(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload))+1, 2, payload+"!")
	artifact := closedAtStamp(payload, 2, manifest.CapturedAt.Add(time.Hour))
	_, err := replayed(manifest, artifact)
	if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "closed on other evidence") {
		t.Fatalf("both wrong: %v", err)
	}
}

func TestTheStampDisagreementNamesItself(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	err := checkReplayRepeatsTheCaptureStamp(manifest, closedAtStamp(payload, 2, manifest.CapturedAt.Add(time.Minute)))
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("want conflict, got %v", err)
	}
	if !strings.Contains(err.Error(), "closed on another capture time") {
		t.Fatalf("message: %q", err.Error())
	}
}

// An open artifact is closed on the manifest in hand, so its stamp is what
// gets filed and there is nothing to repeat.
func TestAnOpenArtifactIsNotHeldToAStamp(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	open := heldArtifact{Exists: true, StepKey: "build",
		TotalBytes: int64(len(payload)), Chunks: 2, CapturedAt: manifest.CapturedAt.Add(time.Hour)}
	outcome, err := artifactClose(manifest, open, digestOf(payload))
	if err != nil || outcome != ArtifactAccepted {
		t.Fatalf("open close: outcome %q err %v", outcome, err)
	}
}
