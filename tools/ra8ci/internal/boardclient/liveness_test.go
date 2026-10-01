// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

func livenessReply(state board.Snapshot, leaseID, holder string, silence, interval time.Duration, overdue bool) map[string]any {
	seen := time.Now().UTC().Add(-silence)
	return map[string]any{
		"snapshot": state,
		"liveness": map[string]any{
			"held": true, "lease_id": leaseID, "holder": holder,
			"last_seen_at": seen, "beat": true,
			"silence_seconds": silence.Seconds(), "interval_seconds": interval.Seconds(),
			"next_beat_by": seen.Add(board.HeartbeatGraceBeats * interval),
			"overdue":      overdue, "expires_at": state.Lease.ExpiresAt,
			"explain": "holder " + holder + " last reported " + silence.String() + " ago",
		},
	}
}

func TestLivenessReadsTheHolderWithoutBeating(t *testing.T) {
	state := activeBoard(t)
	reads, others := 0, 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet && r.URL.Path == "/v1/boards/ek-ra8d2/liveness" {
			reads++
			jsonResponse(w, http.StatusOK,
				livenessReply(state, state.Lease.ID, state.Lease.Holder, 20*time.Second, time.Minute, false))
			return
		}
		// A read must never record a beat, so anything else is a failure.
		others++
		w.WriteHeader(http.StatusInternalServerError)
	})
	defer closeServer()

	snapshot, liveness, err := c.Liveness(context.Background(), "ek-ra8d2")
	if err != nil || reads != 1 || others != 0 {
		t.Fatalf("reading liveness reached the wrong door: reads=%d others=%d err=%v", reads, others, err)
	}
	if snapshot.Version != state.Version || snapshot.Lease == nil || snapshot.Lease.ID != state.Lease.ID {
		t.Fatalf("the read did not carry the board it judged: %+v", snapshot)
	}
	if !liveness.Held || liveness.LeaseID != state.Lease.ID || liveness.Holder != state.Lease.Holder || liveness.Overdue {
		t.Fatalf("the report did not name the holder it read: %+v", liveness)
	}
	// Seconds on the wire, durations in the client.
	if liveness.Silence != 20*time.Second || liveness.Interval != time.Minute {
		t.Fatalf("durations were not decoded from seconds: %+v", liveness)
	}
	if !liveness.NextBeatBy.Equal(liveness.LastSeenAt.Add(board.HeartbeatGraceBeats*time.Minute)) ||
		!liveness.ExpiresAt.Equal(state.Lease.ExpiresAt) {
		t.Fatalf("the times a reader acts on did not survive the wire: %+v", liveness)
	}
}

func TestLivenessRefusesAReportAboutALeaseTheBoardDoesNotRecord(t *testing.T) {
	state := activeBoard(t)
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, http.StatusOK,
			livenessReply(state, "01996f90-3415-7cfe-8ff1-600058131b02", "someone else",
				time.Second, time.Minute, false))
	})
	defer closeServer()
	if _, _, err := c.Liveness(context.Background(), "ek-ra8d2"); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("liveness naming a lease the board does not record was accepted: %v", err)
	}
}

func TestLivenessRefusesAnInvalidBoardBeforeAsking(t *testing.T) {
	asked := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		asked++
		w.WriteHeader(http.StatusOK)
	})
	defer closeServer()
	if _, _, err := c.Liveness(context.Background(), "ek ra8d2"); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("an invalid board ID was read: %v", err)
	}
	if asked != 0 {
		t.Fatalf("an invalid board ID still reached the server: %d requests", asked)
	}
}
