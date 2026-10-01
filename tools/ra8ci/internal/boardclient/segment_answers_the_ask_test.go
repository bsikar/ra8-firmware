// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func segmentAsk(t *testing.T) (LeaseToken, string, string) {
	t.Helper()
	leaseID, err := store.NewID()
	if err != nil {
		t.Fatalf("lease id: %v", err)
	}
	requestID, err := store.NewID()
	if err != nil {
		t.Fatalf("request id: %v", err)
	}
	attemptID, err := store.NewID()
	if err != nil {
		t.Fatalf("attempt id: %v", err)
	}
	token := LeaseToken{
		BoardID:    "ek-ra8d2",
		RequestID:  requestID,
		LeaseID:    leaseID,
		Generation: 7,
		ExpiresAt:  time.Unix(1790000000, 0).UTC(),
		Version:    12,
	}
	return token, attemptID, "flash-restore"
}

func answeredSegment(t *testing.T, token LeaseToken, attemptID, key string) store.BoardSegment {
	t.Helper()
	segmentID, err := store.NewID()
	if err != nil {
		t.Fatalf("segment id: %v", err)
	}
	started := time.Unix(1789999000, 0).UTC()
	return store.BoardSegment{
		ID:               segmentID,
		BoardID:          token.BoardID,
		LeaseID:          token.LeaseID,
		Generation:       token.Generation,
		AttemptID:        attemptID,
		Key:              key,
		StartedAt:        started,
		DeadlineAt:       started.Add(90 * time.Second),
		RecoveryMarginMS: 5000,
	}
}

func TestSegmentAnsweringTheAskIsAccepted(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	if !segmentAnswersTheAsk(answeredSegment(t, token, attemptID, key), token, attemptID, key) {
		t.Fatal("a segment naming this board, lease, generation, attempt and key was refused")
	}
}

func TestSegmentForAnotherBoardIsRefused(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	segment := answeredSegment(t, token, attemptID, key)
	segment.BoardID = "ek-ra8m1"
	if segmentAnswersTheAsk(segment, token, attemptID, key) {
		t.Fatal("a segment on another board was accepted")
	}
}

func TestSegmentForAnotherLeaseIsRefused(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	other, err := store.NewID()
	if err != nil {
		t.Fatalf("other lease id: %v", err)
	}
	segment := answeredSegment(t, token, attemptID, key)
	segment.LeaseID = other
	if segmentAnswersTheAsk(segment, token, attemptID, key) {
		t.Fatal("a segment belonging to another lease was accepted")
	}
}

func TestSegmentOnAnotherGenerationIsRefused(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	for _, generation := range []uint64{0, 6, 8} {
		segment := answeredSegment(t, token, attemptID, key)
		segment.Generation = generation
		if segmentAnswersTheAsk(segment, token, attemptID, key) {
			t.Fatalf("a segment on generation %d was accepted under generation %d", generation, token.Generation)
		}
	}
}

func TestSegmentForAnotherAttemptIsRefused(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	other, err := store.NewID()
	if err != nil {
		t.Fatalf("other attempt id: %v", err)
	}
	segment := answeredSegment(t, token, attemptID, key)
	segment.AttemptID = other
	if segmentAnswersTheAsk(segment, token, attemptID, key) {
		t.Fatal("a segment recording another attempt was accepted")
	}
}

func TestSegmentUnderAnotherKeyIsRefused(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	segment := answeredSegment(t, token, attemptID, key)
	segment.Key = "power-cycle"
	if segmentAnswersTheAsk(segment, token, attemptID, key) {
		t.Fatal("a segment recording another operation key was accepted")
	}
}

func TestSegmentIdentifierMustBeOneTheStoreWouldMint(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	for name, id := range map[string]string{
		"empty":     "",
		"spaces":    "   ",
		"path":      "../../v1/boards/ek-ra8m1/segments/other",
		"newline":   "9f2c\nfinish",
		"oversized": strings.Repeat("a", 4096),
	} {
		segment := answeredSegment(t, token, attemptID, key)
		segment.ID = id
		if segmentAnswersTheAsk(segment, token, attemptID, key) {
			t.Fatalf("a segment identified by %s was accepted", name)
		}
	}
}

func TestSegmentKeyTheStoreWouldNotWriteIsRefused(t *testing.T) {
	token, attemptID, _ := segmentAsk(t)
	for name, key := range map[string]string{
		"empty":     "",
		"control":   "flash\x01restore",
		"newline":   "flash\nrestore",
		"delete":    "flash\x7frestore",
		"oversized": strings.Repeat("k", maxSegmentKeyBytes+1),
	} {
		segment := answeredSegment(t, token, attemptID, key)
		if segmentAnswersTheAsk(segment, token, attemptID, key) {
			t.Fatalf("a segment carrying a %s key was accepted", name)
		}
	}
}

func TestSegmentWithoutStampsIsRefused(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	missingStart := answeredSegment(t, token, attemptID, key)
	missingStart.StartedAt = time.Time{}
	if segmentAnswersTheAsk(missingStart, token, attemptID, key) {
		t.Fatal("a segment with no start was accepted")
	}
	missingDeadline := answeredSegment(t, token, attemptID, key)
	missingDeadline.DeadlineAt = time.Time{}
	if segmentAnswersTheAsk(missingDeadline, token, attemptID, key) {
		t.Fatal("a segment with no deadline was accepted")
	}
}

func TestSegmentDeadlineMustFollowItsStart(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	equal := answeredSegment(t, token, attemptID, key)
	equal.DeadlineAt = equal.StartedAt
	if segmentAnswersTheAsk(equal, token, attemptID, key) {
		t.Fatal("a segment whose deadline equals its start was accepted")
	}
	inverted := answeredSegment(t, token, attemptID, key)
	inverted.DeadlineAt = inverted.StartedAt.Add(-time.Nanosecond)
	if segmentAnswersTheAsk(inverted, token, attemptID, key) {
		t.Fatal("a segment whose deadline precedes its start was accepted")
	}
}

func TestSegmentAnsweredInAnotherZoneIsAccepted(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	segment := answeredSegment(t, token, attemptID, key)
	segment.StartedAt = segment.StartedAt.In(time.FixedZone("bench", -5*3600))
	segment.DeadlineAt = segment.DeadlineAt.In(time.FixedZone("bench", -5*3600))
	if !segmentAnswersTheAsk(segment, token, attemptID, key) {
		t.Fatal("stamps carrying an offset rather than UTC were refused")
	}
}

func TestSegmentWithZeroValuesIsRefused(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	if segmentAnswersTheAsk(store.BoardSegment{}, token, attemptID, key) {
		t.Fatal("an empty segment document was accepted")
	}
}

func TestSegmentRecoveryMarginIsNotJudgedHere(t *testing.T) {
	token, attemptID, key := segmentAsk(t)
	segment := answeredSegment(t, token, attemptID, key)
	segment.RecoveryMarginMS = 0
	if !segmentAnswersTheAsk(segment, token, attemptID, key) {
		t.Fatal("a zero recovery margin was refused; the server owns that arithmetic, not this rule")
	}
}
