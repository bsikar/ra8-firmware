// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func TestBoardLeaseTokenPersistsPrivatelyAndBindsBoard(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "leases")
	requestID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	leaseID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	token := boardclient.LeaseToken{BoardID: "ek-ra8d2", RequestID: requestID,
		LeaseID: leaseID, Generation: 4, ExpiresAt: time.Now().UTC().Add(time.Minute), Version: 12}
	if err := writeBoardLeaseToken(directory, token); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, token.BoardID+".json")
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("lease token mode=%v; want 0600", info.Mode().Perm())
	}
	got, err := readBoardLeaseToken(directory, token.BoardID)
	if err != nil || got != token {
		t.Fatalf("lease token roundtrip: got=%+v err=%v, want=%+v", got, err, token)
	}
	if _, err := readBoardLeaseToken(directory, "another-board"); err == nil {
		t.Fatal("token was read for a different board")
	}
}

func TestBoardLeaseTokenRejectsUnsafeFilePermissions(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "leases")
	requestID, _ := store.NewID()
	leaseID, _ := store.NewID()
	token := boardclient.LeaseToken{BoardID: "ek-ra8d2", RequestID: requestID,
		LeaseID: leaseID, Generation: 1, Version: 5, ExpiresAt: time.Now().UTC().Add(time.Minute)}
	if err := writeBoardLeaseToken(directory, token); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, token.BoardID+".json")
	if err := os.Chmod(path, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := readBoardLeaseToken(directory, token.BoardID); err == nil {
		t.Fatal("world-readable board lease token was accepted")
	}
}

func TestBoardExtendValidatesBeforeLoadingCredentials(t *testing.T) {
	if err := boardExtendCommand(t.Context(), []string{"bad/board", "--why", "debug", "--duration", "30s"}); err == nil {
		t.Fatal("invalid board ID was accepted")
	}
	if err := boardExtendCommand(t.Context(), []string{"ek-ra8d2", "--why", "debug", "--duration", "30ms"}); err == nil {
		t.Fatal("subsecond extension was accepted")
	}
	if err := boardExtendCommand(t.Context(), []string{"ek-ra8d2", "--why", "debug", "--duration", "9h"}); err == nil {
		t.Fatal("extension beyond the bounded maximum was accepted")
	}
}

type fakeBoardLeaseExtender struct {
	gotToken  boardclient.LeaseToken
	gotExpiry time.Time
	gotWhy    string
	result    board.Snapshot
	err       error
}

func (f *fakeBoardLeaseExtender) Extend(_ context.Context, token boardclient.LeaseToken, expiry time.Time,
	why string) (board.Snapshot, error) {
	f.gotToken, f.gotExpiry, f.gotWhy = token, expiry, why
	return f.result, f.err
}

func TestExtendBoardLeaseRefreshesOnlyMatchingDurableToken(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "leases")
	requestID, _ := store.NewID()
	leaseID, _ := store.NewID()
	expires := time.Now().UTC().Add(time.Hour)
	token := boardclient.LeaseToken{BoardID: "ek-ra8d2", RequestID: requestID,
		LeaseID: leaseID, Generation: 3, ExpiresAt: expires, Version: 8}
	if err := writeBoardLeaseToken(directory, token); err != nil {
		t.Fatal(err)
	}
	newExpiry := expires.Add(20 * time.Minute)
	extender := &fakeBoardLeaseExtender{result: board.Snapshot{BoardID: token.BoardID, Version: 9,
		Phase: board.Active, Lease: &board.Lease{ID: leaseID, WaiterID: requestID,
			Generation: token.Generation, ExpiresAt: newExpiry}}}
	snapshot, err := extendBoardLease(context.Background(), extender, directory, token.BoardID, newExpiry, "finish test")
	if err != nil || snapshot.Version != 9 || extender.gotToken != token || extender.gotExpiry != newExpiry || extender.gotWhy != "finish test" {
		t.Fatalf("extension request mismatch: snapshot=%+v sent=%+v expiry=%s why=%q err=%v",
			snapshot, extender.gotToken, extender.gotExpiry, extender.gotWhy, err)
	}
	updated, err := readBoardLeaseToken(directory, token.BoardID)
	if err != nil || updated.ExpiresAt != newExpiry || updated.Version != 9 || updated.Generation != token.Generation {
		t.Fatalf("durable lease token was not refreshed: %+v err=%v", updated, err)
	}
}

func TestExtendBoardLeaseRejectsMismatchedServerLease(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "leases")
	requestID, _ := store.NewID()
	leaseID, _ := store.NewID()
	token := boardclient.LeaseToken{BoardID: "ek-ra8d2", RequestID: requestID,
		LeaseID: leaseID, Generation: 3, Version: 8, ExpiresAt: time.Now().UTC().Add(time.Hour)}
	if err := writeBoardLeaseToken(directory, token); err != nil {
		t.Fatal(err)
	}
	extender := &fakeBoardLeaseExtender{result: board.Snapshot{BoardID: token.BoardID, Version: 9,
		Phase: board.Active, Lease: &board.Lease{ID: "different-lease", WaiterID: requestID,
			Generation: token.Generation, ExpiresAt: time.Now().UTC().Add(2 * time.Hour)}}}
	if _, err := extendBoardLease(context.Background(), extender, directory, token.BoardID,
		time.Now().UTC().Add(2*time.Hour), "finish test"); err == nil {
		t.Fatal("extension for another lease was accepted")
	}
	unchanged, err := readBoardLeaseToken(directory, token.BoardID)
	if err != nil || unchanged != token {
		t.Fatalf("mismatched response changed saved token: %+v err=%v", unchanged, err)
	}
}
