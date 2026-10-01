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

// The queue rows a board keeps in step.
//
// The reducer snapshot is the authority on who is waiting, but operators
// and the API read board_waiters. The projection is what keeps the two
// saying the same thing, so it has to do three jobs: write a row when a
// request joins, close that row when the request leaves, and refuse the
// whole transition when a row no longer matches the snapshot rather than
// quietly letting the two drift apart.
func TestIntegrationTheQueueRowsABoardKeepsInStep(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	boardID := "queue-rows-" + mustID(t)
	submitter := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	now := time.Now().UTC()

	place := func(reason string) board.Waiter {
		return board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: submitter.ID(),
			Class: board.ClassAI, Reason: reason, Duration: time.Minute}
	}
	granted, queued, later := place("granted outright"), place("second in line"), place("third in line")

	// An idle board grants the first request rather than queueing it, so
	// its row is closed as granted on the way in. Only the requests behind
	// it are the queue this test is about.
	if _, _, err := s.ApplyBoardCommand(ctx, submitter, board.Enqueue{Waiter: granted},
		0, nil, nil, now); err != nil {
		t.Fatalf("the first request was not taken: %v", err)
	}
	if _, _, err := s.ApplyBoardCommand(ctx, submitter, board.Enqueue{Waiter: queued},
		1, nil, nil, now.Add(time.Second)); err != nil {
		t.Fatalf("the second request was not queued: %v", err)
	}

	type row struct {
		state    string
		actor    string
		reason   string
		duration int64
		ended    bool
	}
	read := func(t *testing.T, id string) row {
		t.Helper()
		var got row
		if err := pool.QueryRow(ctx, `SELECT state,actor_id,reason,requested_duration_seconds,
			ended_at IS NOT NULL FROM board_waiters WHERE id=$1 AND board_id=$2`, id, boardID).
			Scan(&got.state, &got.actor, &got.reason, &got.duration, &got.ended); err != nil {
			t.Fatalf("no queue row for %s: %v", id, err)
		}
		return got
	}
	absent := func(t *testing.T, id string) bool {
		t.Helper()
		var count int
		if err := pool.QueryRow(ctx, "SELECT count(*) FROM board_waiters WHERE id=$1", id).
			Scan(&count); err != nil {
			t.Fatal(err)
		}
		return count == 0
	}

	t.Run("the rows the first two requests write", func(t *testing.T) {
		held := read(t, granted.ID)
		if held.state != "granted" || !held.ended {
			t.Fatalf("the request that took the board was not closed as granted: %+v", held)
		}
		waiting := read(t, queued.ID)
		if waiting.state != "waiting" || waiting.ended {
			t.Fatalf("the request behind it did not land as waiting: %+v", waiting)
		}
		for _, got := range []row{held, waiting} {
			if got.actor != submitter.ID() || got.duration != 60 {
				t.Fatalf("the queue row does not describe the request: %+v", got)
			}
		}
		if waiting.reason != queued.Reason {
			t.Fatalf("the queued row carries the wrong reason: %+v", waiting)
		}
	})

	t.Run("a queue row that drifted from the snapshot", func(t *testing.T) {
		// Something rewrote the row out from under the snapshot. The next
		// transition verifies every waiter the queue still holds, so it
		// refuses rather than carrying on against a queue it cannot
		// trust, and the request it was asked to add is not written.
		if _, err := pool.Exec(ctx, "UPDATE board_waiters SET reason='rewritten' WHERE id=$1",
			queued.ID); err != nil {
			t.Fatal(err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, "UPDATE board_waiters SET reason=$2 WHERE id=$1",
				queued.ID, queued.Reason); err != nil {
				t.Fatal(err)
			}
		}()
		_, _, err := s.ApplyBoardCommand(ctx, submitter, board.Enqueue{Waiter: later},
			2, nil, nil, now.Add(2*time.Second))
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "projection mismatch") {
			t.Fatalf("a drifted queue row was accepted: %v", err)
		}
		if !absent(t, later.ID) {
			t.Fatalf("the refused transition still wrote a queue row for %s", later.ID)
		}
	})

	t.Run("the row a withdrawn request leaves behind", func(t *testing.T) {
		// With the rows back in step, the same request is accepted, and a
		// withdrawal closes its row as cancelled rather than deleting it:
		// the history is what an operator reads to see who gave up a place.
		if _, _, err := s.ApplyBoardCommand(ctx, submitter, board.Enqueue{Waiter: later},
			2, nil, nil, now.Add(3*time.Second)); err != nil {
			t.Fatalf("the third request was refused: %v", err)
		}
		if _, _, err := s.ApplyBoardCommand(ctx, submitter, board.CancelWaiter{WaiterID: queued.ID},
			3, nil, nil, now.Add(4*time.Second)); err != nil {
			t.Fatalf("the withdrawal was refused: %v", err)
		}
		gone := read(t, queued.ID)
		if gone.state != "cancelled" || !gone.ended {
			t.Fatalf("the withdrawn request was not closed: %+v", gone)
		}
		if gone.reason != queued.Reason {
			t.Fatalf("closing the row rewrote its reason: %+v", gone)
		}
		if stayed := read(t, later.ID); stayed.state != "waiting" || stayed.ended {
			t.Fatalf("the request that stayed was closed too: %+v", stayed)
		}
	})

	t.Run("the places the board still holds", func(t *testing.T) {
		var waiting int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM board_waiters
			WHERE board_id=$1 AND state='waiting'`, boardID).Scan(&waiting); err != nil {
			t.Fatal(err)
		}
		if waiting != 1 {
			t.Fatalf("the board holds %d waiting rows, not 1", waiting)
		}
		snapshot, err := s.GetBoard(ctx, boardID)
		if err != nil {
			t.Fatal(err)
		}
		if len(snapshot.Queue) != 1 || snapshot.Queue[0].ID != later.ID {
			t.Fatalf("the snapshot queue and the rows disagree: %+v", snapshot.Queue)
		}
	})
}
