// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// attemptWindow is one issued attempt with an hour of deadline, the shape
// lockAgentAttempt hands the close path.
func attemptWindow() agentAttempt {
	issued := time.Unix(1790000000, 0).UTC()
	return agentAttempt{
		State:           "running",
		IssuedAt:        issued,
		DeadlineAt:      issued.Add(time.Hour),
		DeadlineSeconds: 3600,
	}
}

// capturedAt is the artifact fixture with its capture stamp moved to when.
func capturedAt(when time.Time) protocol.ArtifactManifest {
	manifest := artifactManifestFixture(5, 1, "first")
	manifest.CapturedAt = when
	return manifest
}

func TestCaptureStampInsideTheAttemptAcceptsACaptureDuringTheRun(t *testing.T) {
	attempt := attemptWindow()
	manifest := capturedAt(attempt.IssuedAt.Add(30 * time.Minute))
	if err := checkCaptureStampSitsInTheAttempt(manifest, attempt); err != nil {
		t.Fatalf("a capture during the attempt: %v", err)
	}
}

func TestCaptureStampInsideTheAttemptAcceptsTheIssueInstant(t *testing.T) {
	attempt := attemptWindow()
	if err := checkCaptureStampSitsInTheAttempt(capturedAt(attempt.IssuedAt), attempt); err != nil {
		t.Fatalf("a capture at the instant the attempt was issued: %v", err)
	}
}

func TestCaptureStampInsideTheAttemptAcceptsTheDeadlineInstant(t *testing.T) {
	attempt := attemptWindow()
	if err := checkCaptureStampSitsInTheAttempt(capturedAt(attempt.DeadlineAt), attempt); err != nil {
		t.Fatalf("a capture at the deadline: %v", err)
	}
}

func TestCaptureStampInsideTheAttemptAllowsClockDisagreementBeforeTheStart(t *testing.T) {
	attempt := attemptWindow()
	manifest := capturedAt(attempt.IssuedAt.Add(-agentEvidenceGrace + time.Second))
	if err := checkCaptureStampSitsInTheAttempt(manifest, attempt); err != nil {
		t.Fatalf("a guest clock a little behind the plane's: %v", err)
	}
}

func TestCaptureStampInsideTheAttemptAllowsClockDisagreementAfterTheDeadline(t *testing.T) {
	attempt := attemptWindow()
	manifest := capturedAt(attempt.DeadlineAt.Add(agentEvidenceGrace - time.Second))
	if err := checkCaptureStampSitsInTheAttempt(manifest, attempt); err != nil {
		t.Fatalf("a guest clock a little ahead of the plane's: %v", err)
	}
}

func TestCaptureStampInsideTheAttemptRefusesACaptureBeforeTheAttemptExisted(t *testing.T) {
	attempt := attemptWindow()
	manifest := capturedAt(attempt.IssuedAt.Add(-24 * time.Hour))
	err := checkCaptureStampSitsInTheAttempt(manifest, attempt)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("a capture from before the attempt was issued: %v", err)
	}
}

func TestCaptureStampInsideTheAttemptRefusesACaptureAfterTheDeadline(t *testing.T) {
	attempt := attemptWindow()
	manifest := capturedAt(attempt.DeadlineAt.Add(24 * time.Hour))
	err := checkCaptureStampSitsInTheAttempt(manifest, attempt)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("a capture from after the deadline: %v", err)
	}
}

func TestCaptureStampInsideTheAttemptRefusesJustOutsideEitherBound(t *testing.T) {
	attempt := attemptWindow()
	early := capturedAt(attempt.IssuedAt.Add(-agentEvidenceGrace - time.Millisecond))
	if err := checkCaptureStampSitsInTheAttempt(early, attempt); !errors.Is(err, ErrConflict) {
		t.Fatalf("a millisecond before the allowance: %v", err)
	}
	late := capturedAt(attempt.DeadlineAt.Add(agentEvidenceGrace + time.Millisecond))
	if err := checkCaptureStampSitsInTheAttempt(late, attempt); !errors.Is(err, ErrConflict) {
		t.Fatalf("a millisecond past the allowance: %v", err)
	}
}

func TestCaptureStampInsideTheAttemptJudgesTheInstantNotTheZone(t *testing.T) {
	attempt := attemptWindow()
	zone := time.FixedZone("UTC-7", -7*60*60)
	manifest := capturedAt(attempt.IssuedAt.Add(30 * time.Minute).In(zone))
	if err := checkCaptureStampSitsInTheAttempt(manifest, attempt); err != nil {
		t.Fatalf("the same instant stated in another zone: %v", err)
	}
}

func TestCaptureStampInsideTheAttemptRefusesTheZeroStampTheWireAlreadyRefuses(t *testing.T) {
	attempt := attemptWindow()
	err := checkCaptureStampSitsInTheAttempt(capturedAt(time.Time{}), attempt)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("a zero capture stamp: %v", err)
	}
}

func TestCaptureStampInsideTheAttemptHoldsAShortAttemptToItsOwnWindow(t *testing.T) {
	attempt := attemptWindow()
	attempt.DeadlineAt = attempt.IssuedAt.Add(30 * time.Second)
	attempt.DeadlineSeconds = 30
	// Inside the hour the longer attempt allowed, outside this one plus its
	// allowance.
	manifest := capturedAt(attempt.IssuedAt.Add(10 * time.Minute))
	if err := checkCaptureStampSitsInTheAttempt(manifest, attempt); !errors.Is(err, ErrConflict) {
		t.Fatalf("a capture past a short attempt's deadline: %v", err)
	}
}
