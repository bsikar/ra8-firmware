// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

// A checkpoint, an extension and a beat all begin the same way: read the board,
// prove the lease this machine kept is still the one the board is holding, and
// only then ask for anything. What happens AFTER that proof is what these pin,
// over a real client against a stand-in plane, with a token actually on disk.
// The existing lease-file tests drive the same helpers through interface fakes,
// which never validate an answer; here every answer goes through the client's
// own checking, which is where a dishonest plane is caught.

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

// leasedBoardFor is the board as the plane reports it while this token holds
// it: the lease the token names, in the phase asked for, with a deadline the
// grant accounts for.
func leasedBoardFor(token boardclient.LeaseToken, phase board.Phase) board.Snapshot {
	snapshot := boardHoldingALease(token.LeaseID, token.RequestID)
	snapshot.BoardID = token.BoardID
	snapshot.Version = token.Version
	snapshot.Phase = phase
	if phase == board.YieldRequested || phase == board.Draining {
		snapshot.Lease.YieldRequestedAt = snapshot.Lease.GrantedAt.Add(time.Minute)
	}
	return snapshot
}

// lengthenedBoard is the same board with its deadline moved out, with the
// extension on record so board.Validate accepts the later expiry.
func lengthenedBoard(token boardclient.LeaseToken, by time.Duration) board.Snapshot {
	snapshot := leasedBoardFor(token, board.Active)
	snapshot.Version = token.Version + 1
	snapshot.Lease.DeadlineVersion = 2
	snapshot.Lease.ExpiresAt = snapshot.Lease.GrantedAt.Add(snapshot.Lease.RequestedDuration + by)
	return snapshot
}

// servingLease answers every board read with state and each POST with the body
// registered for its path, 404ing any path nobody registered so a command
// asking for the wrong thing fails loudly.
func servingLease(t *testing.T, state board.Snapshot, answers map[string]any) http.HandlerFunc {
	t.Helper()
	return func(writer http.ResponseWriter, request *http.Request) {
		writer.Header().Set("Content-Type", "application/json")
		body := state
		if request.Method != http.MethodGet {
			answer, registered := answers[request.URL.Path]
			if !registered {
				writer.WriteHeader(http.StatusNotFound)
				return
			}
			if err := json.NewEncoder(writer).Encode(answer); err != nil {
				t.Errorf("the stand-in plane could not answer %s: %v", request.URL.Path, err)
			}
			return
		}
		if err := json.NewEncoder(writer).Encode(body); err != nil {
			t.Errorf("the stand-in plane could not answer the read: %v", err)
		}
	}
}

// aliveFor is the liveness half of a beat. Both halves are the plane's reading
// of one committed snapshot, so the deadline reported here is the deadline the
// snapshot carries; the client refuses a beat whose two halves disagree.
func aliveFor(snapshot board.Snapshot, seen time.Time) map[string]any {
	return map[string]any{"held": true, "lease_id": snapshot.Lease.ID, "holder": "brighton",
		"last_seen_at": seen, "beat": true, "silence_seconds": 0.0, "interval_seconds": 60.0,
		"next_beat_by": seen.Add(time.Minute), "overdue": false,
		"expires_at": snapshot.Lease.ExpiresAt, "explain": "holder reported alive"}
}

func TestBoardCheckpointSpeaksTheDrainingBoardItWasGiven(t *testing.T) {
	token := heldBoard(t)
	draining := leasedBoardFor(token, board.Draining)
	draining.Version = token.Version + 1
	servingBoard(t, servingLease(t, leasedBoardFor(token, board.YieldRequested),
		map[string]any{"/v1/boards/" + token.BoardID + "/checkpoint": map[string]any{"snapshot": draining}}))

	said, err := spoken(t, func() error {
		return boardCheckpointCommand(context.Background(), []string{token.BoardID})
	})
	if err != nil {
		t.Fatalf("checkpoint: %v", err)
	}
	answered := board.Snapshot{}
	if err := json.Unmarshal([]byte(said), &answered); err != nil {
		t.Fatalf("stdout %q is not a snapshot: %v", said, err)
	}
	if answered.Phase != board.Draining || answered.Lease == nil ||
		answered.Lease.ID != token.LeaseID || answered.Version != token.Version+1 {
		t.Fatalf("operator was shown %+v; want this lease draining at the new version", answered)
	}
}

func TestBoardCheckpointRefusesACheckpointThatLeftTheBoardRunning(t *testing.T) {
	token := heldBoard(t)
	servingBoard(t, servingLease(t, leasedBoardFor(token, board.YieldRequested),
		map[string]any{"/v1/boards/" + token.BoardID + "/checkpoint": map[string]any{
			"snapshot": leasedBoardFor(token, board.Active)}}))

	said, err := spoken(t, func() error {
		return boardCheckpointCommand(context.Background(), []string{token.BoardID})
	})
	if err == nil {
		t.Fatalf("a board still running was reported as checkpointed; said %q", said)
	}
	if !strings.Contains(err.Error(), "board checkpoint:") ||
		!strings.Contains(err.Error(), "another board lease or phase") {
		t.Fatalf("refusal = %v; want the checkpoint named and the phase doubted", err)
	}
	if said != "" {
		t.Fatalf("stdout = %q; want nothing said about a checkpoint that did not happen", said)
	}
}

func TestBoardExtendKeepsTheNewDeadlineWhereTheNextCommandReadsIt(t *testing.T) {
	token := heldBoard(t)
	longer := lengthenedBoard(token, 30*time.Minute)
	servingBoard(t, servingLease(t, leasedBoardFor(token, board.Active),
		map[string]any{"/v1/boards/" + token.BoardID + "/leases/" + token.LeaseID + "/extend": map[string]any{
			"snapshot": longer}}))

	said, err := spoken(t, func() error {
		return boardExtendCommand(context.Background(),
			[]string{token.BoardID, "--why", "the flash write is still running", "--duration", "1h"})
	})
	if err != nil {
		t.Fatalf("extend: %v", err)
	}
	if !strings.Contains(said, token.LeaseID) {
		t.Fatalf("stdout = %q; want the extended lease spoken", said)
	}
	directory, err := currentBoardLeaseDirectory()
	if err != nil {
		t.Fatal(err)
	}
	saved, err := readBoardLeaseToken(directory, token.BoardID)
	if err != nil {
		t.Fatalf("the extended token was not kept: %v", err)
	}
	if saved.Version != longer.Version {
		t.Fatalf("saved version = %d; want the version the extension returned (%d)",
			saved.Version, longer.Version)
	}
	if !saved.ExpiresAt.Equal(longer.Lease.ExpiresAt.UTC()) {
		t.Fatalf("saved deadline = %s; want the one the board granted (%s)",
			saved.ExpiresAt, longer.Lease.ExpiresAt.UTC())
	}
}

func TestBoardHeartbeatWillNotLetABeatLengthenTheLease(t *testing.T) {
	token := heldBoard(t)
	longer := lengthenedBoard(token, 30*time.Minute)
	servingBoard(t, servingLease(t, leasedBoardFor(token, board.Active),
		map[string]any{"/v1/boards/" + token.BoardID + "/leases/" + token.LeaseID + "/heartbeat": map[string]any{
			"snapshot": longer, "liveness": aliveFor(longer, time.Now().UTC())}}))

	said, err := spoken(t, func() error {
		return boardHeartbeatCommand(context.Background(), []string{token.BoardID})
	})
	if err == nil {
		t.Fatalf("a beat lengthened a lease; said %q", said)
	}
	if !strings.Contains(err.Error(), "a beat may not extend a lease") {
		t.Fatalf("refusal = %v; want the beat refused for moving the deadline", err)
	}
	directory, err := currentBoardLeaseDirectory()
	if err != nil {
		t.Fatal(err)
	}
	saved, err := readBoardLeaseToken(directory, token.BoardID)
	if err != nil {
		t.Fatal(err)
	}
	if !saved.ExpiresAt.Equal(token.ExpiresAt) || saved.Version != token.Version {
		t.Fatalf("saved token = %+v; want the refused beat to have changed nothing", saved)
	}
}

func TestBoardHeartbeatKeepsTheVersionTheBeatCameBackWith(t *testing.T) {
	token := heldBoard(t)
	beaten := leasedBoardFor(token, board.Active)
	beaten.Version = token.Version + 1
	servingBoard(t, servingLease(t, leasedBoardFor(token, board.Active),
		map[string]any{"/v1/boards/" + token.BoardID + "/leases/" + token.LeaseID + "/heartbeat": map[string]any{
			"snapshot": beaten, "liveness": aliveFor(beaten, time.Now().UTC())}}))

	if _, err := spoken(t, func() error {
		return boardHeartbeatCommand(context.Background(), []string{token.BoardID})
	}); err != nil {
		t.Fatalf("heartbeat: %v", err)
	}
	directory, err := currentBoardLeaseDirectory()
	if err != nil {
		t.Fatal(err)
	}
	saved, err := readBoardLeaseToken(directory, token.BoardID)
	if err != nil {
		t.Fatal(err)
	}
	if saved.Version != beaten.Version {
		t.Fatalf("saved version = %d; want %d, the version the beat reported",
			saved.Version, beaten.Version)
	}
	if saved.ExpiresAt.After(token.ExpiresAt) {
		t.Fatalf("saved deadline = %s; a beat must never move it past %s",
			saved.ExpiresAt, token.ExpiresAt)
	}
}
