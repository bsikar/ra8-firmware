// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"path/filepath"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type fakeBoardLeaseHeartbeater struct {
	beats    int
	gotToken boardclient.LeaseToken
	snapshot board.Snapshot
	liveness boardclient.HolderLiveness
	err      error
}

func (f *fakeBoardLeaseHeartbeater) Heartbeat(_ context.Context,
	token boardclient.LeaseToken) (board.Snapshot, boardclient.HolderLiveness, error) {
	f.beats++
	f.gotToken = token
	return f.snapshot, f.liveness, f.err
}

func heldBoardLease(t *testing.T) (string, boardclient.LeaseToken) {
	t.Helper()
	directory := filepath.Join(t.TempDir(), "leases")
	requestID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	leaseID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	token := boardclient.LeaseToken{BoardID: "ek-ra8d2", RequestID: requestID, LeaseID: leaseID,
		Generation: 3, Version: 8, ExpiresAt: time.Now().UTC().Add(time.Hour).Truncate(time.Second)}
	if err := writeBoardLeaseToken(directory, token); err != nil {
		t.Fatal(err)
	}
	return directory, token
}

func beatingServer(token boardclient.LeaseToken, version uint64,
	expiry time.Time) *fakeBoardLeaseHeartbeater {
	return &fakeBoardLeaseHeartbeater{
		snapshot: board.Snapshot{BoardID: token.BoardID, Version: version, Phase: board.Active,
			Lease: &board.Lease{ID: token.LeaseID, WaiterID: token.RequestID,
				Generation: token.Generation, ExpiresAt: expiry}},
		liveness: boardclient.HolderLiveness{Held: true, LeaseID: token.LeaseID, Holder: "brighton",
			LastSeenAt: time.Now().UTC(), Beat: true, Interval: time.Minute,
			NextBeatBy: time.Now().UTC().Add(time.Minute), ExpiresAt: expiry,
			Explain: "holder reported alive"},
	}
}

func TestHeartbeatBoardLeaseBeatsFromTheSavedTokenAndRecordsTheVersion(t *testing.T) {
	directory, token := heldBoardLease(t)
	client := beatingServer(token, token.Version+1, token.ExpiresAt)
	snapshot, liveness, err := heartbeatBoardLease(context.Background(), client, directory, token.BoardID)
	if err != nil {
		t.Fatalf("heartbeat: %v", err)
	}
	if client.gotToken != token {
		t.Fatalf("beat sent token=%+v; want %+v", client.gotToken, token)
	}
	if snapshot.Version != token.Version+1 || !liveness.Held || liveness.LeaseID != token.LeaseID {
		t.Fatalf("heartbeat result snapshot=%+v liveness=%+v", snapshot, liveness)
	}
	stored, err := readBoardLeaseToken(directory, token.BoardID)
	if err != nil {
		t.Fatal(err)
	}
	if stored.Version != token.Version+1 {
		t.Fatalf("stored version=%d; want %d", stored.Version, token.Version+1)
	}
	if !stored.ExpiresAt.Equal(token.ExpiresAt) {
		t.Fatalf("a beat moved the recorded deadline: %s -> %s", token.ExpiresAt, stored.ExpiresAt)
	}
}

func TestHeartbeatBoardLeaseRefusesADeadlineABeatWouldHaveBought(t *testing.T) {
	directory, token := heldBoardLease(t)
	client := beatingServer(token, token.Version+1, token.ExpiresAt.Add(time.Hour))
	if _, _, err := heartbeatBoardLease(context.Background(), client, directory, token.BoardID); err == nil {
		t.Fatal("a heartbeat that lengthened the lease was accepted")
	}
	stored, err := readBoardLeaseToken(directory, token.BoardID)
	if err != nil {
		t.Fatal(err)
	}
	if stored != token {
		t.Fatalf("refused beat still rewrote the token: %+v", stored)
	}
}

func TestHeartbeatBoardLeaseRecordsADeadlineThatCameBackShorter(t *testing.T) {
	directory, token := heldBoardLease(t)
	shorter := token.ExpiresAt.Add(-15 * time.Minute)
	client := beatingServer(token, token.Version+1, shorter)
	if _, _, err := heartbeatBoardLease(context.Background(), client, directory, token.BoardID); err != nil {
		t.Fatalf("heartbeat: %v", err)
	}
	stored, err := readBoardLeaseToken(directory, token.BoardID)
	if err != nil {
		t.Fatal(err)
	}
	if !stored.ExpiresAt.Equal(shorter) {
		t.Fatalf("stored deadline=%s; want the server's %s", stored.ExpiresAt, shorter)
	}
}

func TestHeartbeatBoardLeaseRefusesAnAnswerAboutAnotherLease(t *testing.T) {
	directory, token := heldBoardLease(t)
	client := beatingServer(token, token.Version+1, token.ExpiresAt)
	client.snapshot.Lease.ID = "another-lease"
	if _, _, err := heartbeatBoardLease(context.Background(), client, directory, token.BoardID); err == nil {
		t.Fatal("a heartbeat naming another lease was accepted")
	}
	client = beatingServer(token, token.Version+1, token.ExpiresAt)
	client.liveness.LeaseID = "another-lease"
	if _, _, err := heartbeatBoardLease(context.Background(), client, directory, token.BoardID); err == nil {
		t.Fatal("a liveness report about another lease was accepted")
	}
	client = beatingServer(token, token.Version+1, token.ExpiresAt)
	client.liveness.Held = false
	if _, _, err := heartbeatBoardLease(context.Background(), client, directory, token.BoardID); err == nil {
		t.Fatal("a report that the board is unheld was accepted as this holder beating")
	}
}

func TestHeartbeatBoardLeaseSendsNothingWithoutASavedToken(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "leases")
	client := &fakeBoardLeaseHeartbeater{}
	if _, _, err := heartbeatBoardLease(context.Background(), client, directory, "ek-ra8d2"); err == nil {
		t.Fatal("a beat was accepted with no lease token on this machine")
	}
	if client.beats != 0 {
		t.Fatalf("beats=%d; want none sent without a token", client.beats)
	}
}

func TestBoardHeartbeatAndLivenessValidateBeforeLoadingCredentials(t *testing.T) {
	if err := boardHeartbeatCommand(t.Context(), []string{"bad/board"}); err == nil {
		t.Fatal("invalid board ID was accepted by heartbeat")
	}
	if err := boardHeartbeatCommand(t.Context(), []string{}); err == nil {
		t.Fatal("heartbeat with no board was accepted")
	}
	if err := boardHeartbeatCommand(t.Context(), []string{"ek-ra8d2", "--why", "debug"}); err == nil {
		t.Fatal("heartbeat took a flag; a beat carries no reason")
	}
	if err := boardLivenessCommand(t.Context(), []string{"bad/board"}); err == nil {
		t.Fatal("invalid board ID was accepted by liveness")
	}
}

func TestBoardLivenessLineLeavesAbsentInstantsNull(t *testing.T) {
	line := boardLivenessLineFrom(boardclient.HolderLiveness{Held: false, Interval: time.Minute,
		Silence: 90 * time.Second, Explain: "board is not held"})
	if line.LastSeenAt != nil || line.NextBeatBy != nil || line.ExpiresAt != nil {
		t.Fatalf("absent instants were rendered: %+v", line)
	}
	if line.SilenceSeconds != 90 || line.IntervalSeconds != 60 {
		t.Fatalf("durations rendered as %ds silence / %ds interval", line.SilenceSeconds, line.IntervalSeconds)
	}
}
