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
	"github.com/jackc/pgx/v5"
)

// inOwnTx hands the package-private projection its own transaction, the
// way persistBoardTransition would, so the projection is judged on its
// own terms rather than through a command that has already refused.
func inOwnTx(t *testing.T, ctx context.Context, s *Store, body func(tx pgx.Tx)) {
	t.Helper()
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	body(tx)
}

// The waiter a queue will not project.
//
// A queued waiter is the durable half of an in-memory decision: the
// reducer has already accepted it, so a projection that silently wrote
// nothing would leave a board whose queue the database disagrees with.
// Every way the row can fail has to come back as a conflict instead.
func TestIntegrationTheWaiterAQueueWillNotProject(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	boardID := "board-waiter-" + mustID(t)
	if _, err := pool.Exec(ctx, `INSERT INTO boards (id,generation,state,version)
		VALUES ($1,0,'available',0)`, boardID); err != nil {
		t.Fatal(err)
	}
	sound := func() board.Waiter {
		return board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: "actor-" + mustID(t),
			Class: board.ClassAI, Reason: "projected waiter", Duration: 90 * time.Second}
	}
	queuedAt := time.Now().UTC()

	unprojectable := map[string]board.Waiter{}
	w := sound()
	w.ID = "not-a-uuid"
	unprojectable["a waiter with no usable identifier"] = w
	w = sound()
	w.LeaseID = ""
	unprojectable["a waiter holding no lease identifier"] = w
	for name, waiter := range unprojectable {
		t.Run(name, func(t *testing.T) {
			inOwnTx(t, ctx, s, func(tx pgx.Tx) {
				err := insertBoardWaiter(ctx, tx, boardID, waiter, queuedAt)
				if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "board waiter cannot be projected") {
					t.Fatalf("the projection accepted it: %v", err)
				}
			})
		})
	}

	t.Run("a waiter queued at no time at all", func(t *testing.T) {
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			err := insertBoardWaiter(ctx, tx, boardID, sound(), time.Time{})
			if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "board waiter cannot be projected") {
				t.Fatalf("a waiter was queued at the zero time: %v", err)
			}
		})
	})

	// The refusals below reach the database and come back from the row's
	// own constraints, which is the half a guard cannot see.
	t.Run("a waiter asking for no time on the board", func(t *testing.T) {
		waiter := sound()
		waiter.Duration = 0
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			err := insertBoardWaiter(ctx, tx, boardID, waiter, queuedAt)
			if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "project waiter") {
				t.Fatalf("a zero-duration waiter was projected: %v", err)
			}
		})
	})

	t.Run("a waiter with no stated reason", func(t *testing.T) {
		waiter := sound()
		waiter.Reason = ""
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			err := insertBoardWaiter(ctx, tx, boardID, waiter, queuedAt)
			if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "project waiter") {
				t.Fatalf("a reasonless waiter was projected: %v", err)
			}
		})
	})

	t.Run("the same waiter projected twice", func(t *testing.T) {
		waiter := sound()
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			if err := insertBoardWaiter(ctx, tx, boardID, waiter, queuedAt); err != nil {
				t.Fatalf("the first projection failed: %v", err)
			}
			err := insertBoardWaiter(ctx, tx, boardID, waiter, queuedAt)
			if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "project waiter") {
				t.Fatalf("one waiter was queued twice: %v", err)
			}
		})
	})

	t.Run("the waiter the queue does project", func(t *testing.T) {
		waiter := sound()
		waiter.Class = board.ClassHuman
		waiter.Duration = 90*time.Second + time.Millisecond
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			if err := insertBoardWaiter(ctx, tx, boardID, waiter, queuedAt); err != nil {
				t.Fatalf("a sound waiter was refused: %v", err)
			}
			var actor, priority, reason, state, request string
			var seconds int64
			if err := tx.QueryRow(ctx, `SELECT actor_id,priority,reason,requested_duration_seconds,state,request_id
				FROM board_waiters WHERE id=$1 AND board_id=$2`, waiter.ID, boardID).
				Scan(&actor, &priority, &reason, &seconds, &state, &request); err != nil {
				t.Fatalf("the projected waiter is not readable: %v", err)
			}
			// A fraction of a second is rounded up, never down: a waiter
			// must never be recorded as asking for less than it asked for.
			if actor != waiter.Holder || priority != "human" || reason != waiter.Reason ||
				seconds != 91 || state != "waiting" || request != waiter.ID {
				t.Fatalf("the waiter projected as actor=%s priority=%s reason=%q seconds=%d state=%s request=%s",
					actor, priority, reason, seconds, state, request)
			}
			// verifyBoardWaiter is the read the transition makes of its own
			// projection, so the two have to agree about this row.
			if err := verifyBoardWaiter(ctx, tx, boardID, waiter); err != nil {
				t.Fatalf("the transition would not recognise its own waiter: %v", err)
			}
		})
	})
}

// The actor a board call revalidates.
//
// An actor authorized at the door is revalidated inside every board
// transaction, because a grant can be revoked between the handshake and
// the write. The three ways an actor can arrive unusable are refused
// before the grant is even read.
func TestIntegrationTheActorABoardCallRevalidates(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	boardID := "board-actor-" + mustID(t)

	denied := map[string]BoardActor{
		"an actor carrying no certificate": {id: "someone", kind: "board_agent", role: "board_agent",
			boardID: boardID, repository: boardTestRepo},
		"an actor bound to no repository": {id: "someone", kind: "board_agent", role: "board_agent",
			boardID: boardID, certificate: strings.Repeat("c", 64)},
		"the server's own identity claimed under another name": {id: "ra8ci-agent", kind: "system",
			role: "system", boardID: boardID},
		"the server's own name claimed under another role": {id: "ra8ci-server", kind: "system",
			role: "operator", boardID: boardID},
	}
	for name, actor := range denied {
		t.Run(name, func(t *testing.T) {
			inOwnTx(t, ctx, s, func(tx pgx.Tx) {
				if err := revalidateBoardActor(ctx, tx, actor); !errors.Is(err, ErrDenied) {
					t.Fatalf("the actor was revalidated: %v", err)
				}
			})
		})
	}

	t.Run("the server itself", func(t *testing.T) {
		// The sweeper has no certificate to present, so its identity is
		// the whole of its authority and all three parts must match.
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			actor := BoardActor{id: "ra8ci-server", kind: "system", role: "system", boardID: boardID}
			if err := revalidateBoardActor(ctx, tx, actor); err != nil {
				t.Fatalf("the server could not revalidate itself: %v", err)
			}
		})
	})

	t.Run("a board agent whose grant still stands", func(t *testing.T) {
		agent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			if err := revalidateBoardActor(ctx, tx, agent); err != nil {
				t.Fatalf("a granted board agent was refused: %v", err)
			}
		})
		if _, err := pool.Exec(ctx, "UPDATE api_principals SET revoked_at=clock_timestamp() WHERE principal_id=$1",
			agent.ID()); err != nil {
			t.Fatal(err)
		}
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			if err := revalidateBoardActor(ctx, tx, agent); !errors.Is(err, ErrDenied) {
				t.Fatalf("a revoked board agent still revalidated: %v", err)
			}
		})
	})
}
