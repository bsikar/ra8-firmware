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

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The history a board transition writes, and what it will not write.
//
// Every accepted board command lands through one function: the
// snapshot is compare-and-set forward, the projections are updated,
// and the events are appended under numbers that continue the board's
// own history. The numbering is the part worth holding still, because
// a board's event log is append-only and a repeated or skipped number
// cannot be repaired afterwards. Each case runs in its own transaction
// and rolls back, so the board itself is never disturbed.
func TestIntegrationTheHistoryATransitionWrites(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	s, pool, human, _, active, _, now := activeTestBoard(t, ctx)

	// An inert event: the generation reconciliation carries no waiter,
	// lease or session projection, so what it changes is the history
	// alone.
	beat := func(at time.Time, generation uint64) board.Event {
		return board.Event{Kind: board.GenerationReconciled, At: at, BoardID: active.BoardID,
			Actor: human.id, Generation: generation, Reason: "a transition under test"}
	}

	highest := func(t *testing.T) int64 {
		t.Helper()
		var seq int64
		if err := pool.QueryRow(ctx, `SELECT COALESCE(MAX(event_seq),0) FROM board_events
			WHERE board_id=$1`, active.BoardID).Scan(&seq); err != nil {
			t.Fatal(err)
		}
		return seq
	}

	t.Run("a transition against a version the board has left", func(t *testing.T) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer func() { _ = tx.Rollback(ctx) }()
		stale := active
		stale.Version = active.Version + 7
		moved := active
		moved.Version = active.Version + 8
		err = persistBoardTransition(ctx, tx, stale, moved, board.Tick{},
			[]board.Event{beat(now.Add(time.Second), active.Generation)})
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "board snapshot CAS") {
			t.Fatalf("a transition from a version the board has left was written: %v", err)
		}
		// The CAS is the first thing the write does, so nothing behind it
		// may have run.
		var written int
		if err := tx.QueryRow(ctx, `SELECT count(*) FROM board_events WHERE board_id=$1
			AND reason='a transition under test'`, active.BoardID).Scan(&written); err != nil {
			t.Fatal(err)
		}
		if written != 0 {
			t.Fatalf("a refused transition still wrote %d events", written)
		}
	})

	t.Run("the numbers a transition continues from", func(t *testing.T) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer func() { _ = tx.Rollback(ctx) }()
		before := highest(t)
		moved := active
		moved.Version = active.Version + 1
		if err := persistBoardTransition(ctx, tx, active, moved, board.Tick{}, []board.Event{
			beat(now.Add(time.Second), active.Generation),
			beat(now.Add(2*time.Second), active.Generation),
		}); err != nil {
			t.Fatalf("a well-formed transition was refused: %v", err)
		}
		rows, err := tx.Query(ctx, `SELECT event_seq,snapshot_version,kind FROM board_events
			WHERE board_id=$1 AND reason='a transition under test' ORDER BY event_seq`, active.BoardID)
		if err != nil {
			t.Fatal(err)
		}
		defer rows.Close()
		var seqs []int64
		for rows.Next() {
			var seq, version int64
			var kind string
			if err := rows.Scan(&seq, &version, &kind); err != nil {
				t.Fatal(err)
			}
			if uint64(version) != moved.Version || kind != string(board.GenerationReconciled) {
				t.Fatalf("event %d was written as version %d kind %q", seq, version, kind)
			}
			seqs = append(seqs, seq)
		}
		if err := rows.Err(); err != nil {
			t.Fatal(err)
		}
		// Two events, numbered straight on from whatever the board had
		// already recorded, never from one.
		if len(seqs) != 2 || seqs[0] != before+1 || seqs[1] != before+2 {
			t.Fatalf("the history jumped: had %d, wrote %v", before, seqs)
		}
		var version int64
		if err := tx.QueryRow(ctx, "SELECT version FROM board_snapshots WHERE board_id=$1",
			active.BoardID).Scan(&version); err != nil {
			t.Fatal(err)
		}
		if uint64(version) != moved.Version {
			t.Fatalf("the snapshot stayed at %d", version)
		}
	})

	t.Run("an event the history cannot carry", func(t *testing.T) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer func() { _ = tx.Rollback(ctx) }()
		// The snapshot table accepts version zero; the event log does
		// not, because an event has to name the version it belongs to.
		// So a transition landing there is refused at the append rather
		// than quietly dropping its events.
		zeroed := active
		zeroed.Version = 0
		err = persistBoardTransition(ctx, tx, active, zeroed, board.Tick{},
			[]board.Event{beat(now.Add(time.Second), active.Generation)})
		if !errors.Is(err, ErrUnavailable) || !strings.Contains(err.Error(), "board event insert") {
			t.Fatalf("the history took an event it cannot carry: %v", err)
		}
	})

	t.Run("the board the rolled-back transitions left alone", func(t *testing.T) {
		snapshot, err := s.GetBoard(ctx, active.BoardID)
		if err != nil {
			t.Fatal(err)
		}
		if snapshot.Version != active.Version || snapshot.Phase != active.Phase {
			t.Fatalf("a rolled-back transition reached the board: %+v", snapshot)
		}
		var written int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM board_events WHERE board_id=$1
			AND reason='a transition under test'`, active.BoardID).Scan(&written); err != nil {
			t.Fatal(err)
		}
		if written != 0 {
			t.Fatalf("%d events under test survived their rollback", written)
		}
	})
}
