//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// The beat a board snapshot will not take.
//
// A heartbeat is the one board write that carries no events and no
// projection: it just moves the snapshot forward. That makes its
// compare-and-set the only thing standing between two writers, so it
// has to refuse a beat written against a version the board has already
// left, and against a board that does not exist at all. Each case runs
// in its own transaction and rolls back, so the board itself is never
// disturbed.
func TestIntegrationTheBeatABoardSnapshotWillNotTake(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	s, pool, _, _, active, _, _ := activeTestBoard(t, ctx)

	t.Run("a beat against the version the board is on", func(t *testing.T) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer func() { _ = tx.Rollback(ctx) }()
		before := active
		moved := active
		moved.Version = active.Version + 1
		if err := persistBoardLiveness(ctx, tx, before, moved); err != nil {
			t.Fatalf("a beat on the current version was refused: %v", err)
		}
		var stored int64
		if err := tx.QueryRow(ctx, "SELECT version FROM board_snapshots WHERE board_id=$1",
			active.BoardID).Scan(&stored); err != nil {
			t.Fatal(err)
		}
		if uint64(stored) != moved.Version {
			t.Fatalf("the beat stored version %d, not %d", stored, moved.Version)
		}
	})

	t.Run("a beat against a version the board has left", func(t *testing.T) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer func() { _ = tx.Rollback(ctx) }()
		stale := active
		stale.Version = active.Version + 7
		moved := active
		moved.Version = active.Version + 8
		err = persistBoardLiveness(ctx, tx, stale, moved)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "board snapshot CAS") {
			t.Fatalf("a beat on a version the board has left was taken: %v", err)
		}
	})

	t.Run("a beat for a board that does not exist", func(t *testing.T) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer func() { _ = tx.Rollback(ctx) }()
		before := active
		before.BoardID = "board-" + mustID(t)
		moved := before
		moved.Version = before.Version + 1
		err = persistBoardLiveness(ctx, tx, before, moved)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "board snapshot CAS") {
			t.Fatalf("a beat invented a board: %v", err)
		}
	})

	t.Run("the board the rolled-back beats left alone", func(t *testing.T) {
		// Every case above ran in a transaction that was rolled back, so
		// the board must still read exactly as the fixture left it.
		snapshot, err := s.GetBoard(ctx, active.BoardID)
		if err != nil {
			t.Fatal(err)
		}
		if snapshot.Version != active.Version || snapshot.Phase != active.Phase {
			t.Fatalf("a rolled-back beat reached the board: %+v", snapshot)
		}
	})
}
